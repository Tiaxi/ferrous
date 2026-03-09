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
    property real positionSmoothingLastMs: 0
    property string positionSmoothingTrackPath: ""
    property real albumArtZoom: 1.0
    property real albumArtPanX: 0.0
    property real albumArtPanY: 0.0
    property string pendingFolderDialogContext: ""
    property string pendingFileDialogContext: ""
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
    readonly property var uiBridge: bridge ? bridge : bridgeFallback
    readonly property var globalSearchModelApi: (uiBridge
        && uiBridge.globalSearchModel
        && uiBridge.globalSearchModel.nextSelectableIndex)
        ? uiBridge.globalSearchModel
        : globalSearchModelFallback

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
        property string diagnosticsText: ""
        property string diagnosticsLogPath: ""
        property bool connected: false
        signal snapshotChanged()
        signal analysisChanged()
        signal libraryTreeFrameReceived(int version, var treeBytes)
        signal globalSearchResultsChanged()
        signal diagnosticsChanged()
        signal bridgeError(string message)
        function play() {}
        function pause() {}
        function stop() {}
        function next() {}
        function previous() {}
        function seek(seconds) {}
        function setVolume(value) {}
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
        function addLibraryRoot(path) {}
        function removeLibraryRoot(path) {}
        function rescanLibraryRoot(path) {}
        function rescanAllLibraryRoots() {}
        function setLibraryNodeExpanded(key, expanded) {}
        function setLibrarySortMode(mode) {}
        function setGlobalSearchQuery(query) {}
        function openInFileBrowser(path) {}
        function openContainingFolder(path) {}
        function scanRoot(path) {}
        function scanDefaultMusicRoot() {}
        function requestSnapshot() {}
        function shutdown() {}
        function clearDiagnostics() {}
        function reloadDiagnosticsFromDisk() {}
        function takeSpectrogramRowsDeltaPacked() { return ({ rows: 0, bins: 0, data: "" }) }
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
    readonly property int globalSearchTrackLengthColumnWidth: Math.max(
        34,
        Math.ceil(playlistOrderFontMetrics.boundingRect("00:00").width + 6))
    readonly property int globalSearchTrackGenreColumnWidth: Math.max(
        116,
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

    Timer {
        id: positionSmoothingTimer
        interval: 16
        repeat: true
        running: !seekSlider.pressed
            && uiBridge.playbackState === "Playing"
            && root.visualFeedsEnabled
            && root.positionSmoothingPrimed
        onTriggered: {
            const nowMs = Date.now()
            if (root.positionSmoothingLastMs <= 0) {
                root.positionSmoothingLastMs = nowMs
            }
            const elapsed = Math.max(0.0, Math.min(1.5, (nowMs - root.positionSmoothingLastMs) / 1000.0))
            const predicted = root.positionSmoothingAnchorSeconds + elapsed
            const duration = Math.max(uiBridge.durationSeconds, 0)
            if (duration > 0) {
                root.displayedPositionSeconds = Math.min(duration, predicted)
            } else {
                root.displayedPositionSeconds = Math.max(0.0, predicted)
            }
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

    function openAlbumArtViewer() {
        if (!uiBridge.currentTrackCoverPath || uiBridge.currentTrackCoverPath.length === 0) {
            return
        }
        albumArtZoom = 1.0
        albumArtPanX = 0.0
        albumArtPanY = 0.0
        albumArtViewer.initialViewPending = true
        albumArtViewer.open()
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
        onTriggered: spectrogramItem.reset()
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
        width: Math.min(760, root.width - 80)
        height: Math.min(620, root.height - 80)
        enter: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        exit: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }

        contentItem: ScrollView {
            id: preferencesScrollView
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: preferencesScrollView.availableWidth
                spacing: 12

                GroupBox {
                    title: "Library"
                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

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
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: "Album Sort:" }
                            ComboBox {
                                model: ["Year", "Title"]
                                currentIndex: Math.max(0, Math.min(1, uiBridge.librarySortMode))
                                onActivated: uiBridge.setLibrarySortMode(currentIndex)
                                Layout.preferredWidth: 160
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: uiBridge.libraryRoots.length === 0
                                ? "No library roots configured."
                                : "Configured roots:"
                            color: Kirigami.Theme.disabledTextColor
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(180, 30 * Math.max(1, uiBridge.libraryRoots.length))
                            color: Qt.rgba(0, 0, 0, 0.03)
                            border.color: Qt.rgba(0, 0, 0, 0.08)
                            visible: uiBridge.libraryRoots.length > 0

                            ListView {
                                anchors.fill: parent
                                clip: true
                                model: uiBridge.libraryRoots
                                boundsBehavior: Flickable.StopAtBounds
                                boundsMovement: Flickable.StopAtBounds
                                flickDeceleration: root.snappyScrollFlickDeceleration
                                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                                pixelAligned: true
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.NoButton
                                    preventStealing: true
                                    onWheel: function(wheel) {
                                        root.stepScrollView(parent, wheel, 30, 3)
                                    }
                                }
                                delegate: RowLayout {
                                    width: ListView.view.width
                                    spacing: 6
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData
                                        elide: Text.ElideMiddle
                                    }
                                    ToolButton {
                                        text: "Open"
                                        onClicked: uiBridge.openInFileBrowser(modelData)
                                    }
                                    ToolButton {
                                        text: "Rescan"
                                        onClicked: uiBridge.rescanLibraryRoot(modelData)
                                    }
                                    ToolButton {
                                        text: "Remove"
                                        onClicked: uiBridge.removeLibraryRoot(modelData)
                                    }
                                }
                            }
                        }
                    }
                }

                GroupBox {
                    title: "Visualization"
                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: "dB Range:" }
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

                GroupBox {
                    title: "Last.fm"
                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

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
                        }
                    }
                }

                GroupBox {
                    title: "System Media"
                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

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

    Dialog {
        id: globalSearchDialog
        modal: true
        title: "Global Search"
        standardButtons: Dialog.Close
        width: Math.min(1080, root.width - 80)
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
                            }

                            RowLayout {
                                visible: rowKind === "columns" && rowTypeValue === "album"
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: ""; Layout.preferredWidth: 26; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: "Title"; Layout.fillWidth: true; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: "Artist"; Layout.preferredWidth: 170; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label {
                                    text: "Year"
                                    Layout.preferredWidth: 52
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
                                Label { text: "#"; Layout.preferredWidth: 34; font.weight: Font.DemiBold; color: root.uiMutedTextColor; horizontalAlignment: Text.AlignRight }
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
                                    Layout.preferredWidth: 34
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label { text: "Title"; Layout.fillWidth: true; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: "Artist"; Layout.preferredWidth: 160; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: ""; Layout.preferredWidth: 20; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: "Album"; Layout.preferredWidth: 182; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label {
                                    text: "Year"
                                    Layout.preferredWidth: 52
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
                                Label { text: "Length"; Layout.preferredWidth: 76; font.weight: Font.DemiBold; color: root.uiMutedTextColor; horizontalAlignment: Text.AlignRight }
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
                            }
                        }

                        Component {
                            id: globalSearchAlbumItemComponent
                            RowLayout {
                                spacing: 8
                                Item {
                                    Layout.preferredWidth: 26
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
                                    Layout.preferredWidth: 170
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: yearValue !== undefined && yearValue !== null ? yearValue : ""
                                    Layout.preferredWidth: 52
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: genreValue || ""
                                    Layout.preferredWidth: 120
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: countValue !== undefined ? countValue : ""
                                    Layout.preferredWidth: 34
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
                                    Layout.preferredWidth: 34
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
                                    Layout.preferredWidth: 160
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Item {
                                    Layout.preferredWidth: 20
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
                                    Layout.preferredWidth: 182
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: yearValue !== undefined && yearValue !== null ? yearValue : ""
                                    Layout.preferredWidth: 52
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
                    uiBridge.addLibraryRoot(localPath)
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
                            enabled: uiBridge.currentTrackCoverPath.length > 0
                            acceptedButtons: Qt.LeftButton
                            onDoubleClicked: function(mouse) {
                                if (mouse.button === Qt.LeftButton) {
                                    root.openAlbumArtViewer()
                                }
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

                    SpectrogramItem {
                        id: spectrogramItem
                        anchors.fill: parent
                        maxColumns: Math.max(640, Math.floor(width))
                        dbRange: uiBridge.dbRange
                        logScale: uiBridge.logScale
                        showFpsOverlay: uiBridge.showFps
                        sampleRateHz: uiBridge.sampleRateHz
                    }
                }
            }
        }
    }

    Popup {
        id: albumArtViewer
        parent: Overlay.overlay
        property bool initialViewPending: false
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
        background: Rectangle {
            color: "#000000"
            opacity: 0.87
        }
        function clampPan() {
            const scaledW = albumArtTransform.width * root.albumArtZoom
            const scaledH = albumArtTransform.height * root.albumArtZoom
            const limitX = Math.max(0, (scaledW - albumArtViewport.width) / 2)
            const limitY = Math.max(0, (scaledH - albumArtViewport.height) / 2)
            root.albumArtPanX = Math.max(-limitX, Math.min(limitX, root.albumArtPanX))
            root.albumArtPanY = Math.max(-limitY, Math.min(limitY, root.albumArtPanY))
        }
        function isPointOnImage(item, x, y) {
            const p = albumArtImageFull.mapFromItem(item, x, y)
            const xOff = (albumArtImageFull.width - albumArtImageFull.paintedWidth) / 2
            const yOff = (albumArtImageFull.height - albumArtImageFull.paintedHeight) / 2
            return p.x >= xOff
                && p.y >= yOff
                && p.x <= xOff + albumArtImageFull.paintedWidth
                && p.y <= yOff + albumArtImageFull.paintedHeight
        }
        function applyInitialView() {
            if (!initialViewPending) {
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
            clampPan()
            initialViewPending = false
        }
        onOpened: {
            applyInitialView()
        }
        onWidthChanged: applyInitialView()
        onHeightChanged: applyInitialView()

        MouseArea {
            id: albumArtDismissArea
            z: 0
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onClicked: albumArtViewer.close()
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
                onClicked: albumArtViewer.close()
            }
        }

        Item {
            id: albumArtViewport
            z: 1
            anchors.fill: parent
            anchors.margins: 0
            clip: true
            onWidthChanged: albumArtViewer.applyInitialView()
            onHeightChanged: albumArtViewer.applyInitialView()

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
                    source: uiBridge.currentTrackCoverPath
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    asynchronous: true
                    cache: true
                    retainWhileLoading: true
                    onStatusChanged: albumArtViewer.applyInitialView()
                }
            }

            MouseArea {
                id: albumArtPanArea
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                preventStealing: true
                property real lastX: 0
                property real lastY: 0
                cursorShape: root.albumArtZoom > 1.0 ? Qt.OpenHandCursor : Qt.ArrowCursor
                onPressed: function(mouse) {
                    if (!albumArtViewer.isPointOnImage(albumArtPanArea, mouse.x, mouse.y)) {
                        albumArtViewer.close()
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
                    albumArtViewer.clampPan()
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
                        albumArtViewer.clampPan()
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
                    albumArtViewer.clampPan()
                    wheel.accepted = true
                }
            }
        }
    }

    onClosing: function(close) { uiBridge.shutdown() }

    Connections {
        target: uiBridge
        function applyAnalysisDelta() {
            if (uiBridge.spectrogramReset && root.visualFeedsEnabled) {
                spectrogramItem.reset()
            }
            const delta = uiBridge.takeSpectrogramRowsDeltaPacked()
            if (root.visualFeedsEnabled && delta.rows > 0 && delta.bins > 0) {
                spectrogramItem.appendPackedRows(delta.data, delta.rows, delta.bins)
            }
        }
        function onSnapshotChanged() {
            root.syncMutedVolumeState()
            const incomingPosition = uiBridge.positionSeconds
            const trackChanged = root.positionSmoothingTrackPath !== uiBridge.currentTrackPath
            const nowMs = Date.now()
            if (uiBridge.playbackState !== "Playing") {
                root.displayedPositionSeconds = incomingPosition
                root.positionSmoothingPrimed = false
                root.positionSmoothingAnchorSeconds = incomingPosition
                root.positionSmoothingLastMs = nowMs
                root.positionSmoothingTrackPath = uiBridge.currentTrackPath
            } else if (!root.positionSmoothingPrimed || trackChanged) {
                root.displayedPositionSeconds = incomingPosition
                root.positionSmoothingPrimed = true
                root.positionSmoothingAnchorSeconds = incomingPosition
                root.positionSmoothingLastMs = nowMs
                root.positionSmoothingTrackPath = uiBridge.currentTrackPath
            } else {
                const elapsed = Math.max(0.0, Math.min(1.5, (nowMs - root.positionSmoothingLastMs) / 1000.0))
                const predicted = root.positionSmoothingAnchorSeconds + elapsed
                const drift = incomingPosition - predicted
                if (Math.abs(drift) > 0.20) {
                    root.displayedPositionSeconds = incomingPosition
                } else {
                    const corrected = predicted + drift * 0.20
                    const duration = Math.max(uiBridge.durationSeconds, 0)
                    if (duration > 0) {
                        root.displayedPositionSeconds = Math.min(duration, Math.max(0.0, corrected))
                    } else {
                        root.displayedPositionSeconds = Math.max(0.0, corrected)
                    }
                }
                root.positionSmoothingAnchorSeconds = incomingPosition
                root.positionSmoothingLastMs = nowMs
                root.positionSmoothingTrackPath = uiBridge.currentTrackPath
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

    Component.onCompleted: {
        root.requestLibraryTreeApply(uiBridge.libraryVersion, uiBridge.libraryTreeBinary || "")
        root.lastSeenQueueVersion = uiBridge.queueVersion
        root.lastAutoCenterPlaybackState = uiBridge.playbackState
        root.lastAutoCenterTrackPath = uiBridge.currentTrackPath
        root.displayedPositionSeconds = uiBridge.positionSeconds
        root.syncMutedVolumeState()
        root.positionSmoothingPrimed = uiBridge.playbackState === "Playing"
        root.positionSmoothingAnchorSeconds = uiBridge.positionSeconds
        root.positionSmoothingLastMs = Date.now()
        root.positionSmoothingTrackPath = uiBridge.currentTrackPath
        root.syncQueueSelectionToCurrentQueue()
        root.lastSyncedBridgeSelectedQueueIndex = uiBridge.selectedQueueIndex
        root.syncLibrarySelectionToVisibleRows()
        root.syncGlobalSearchSelectionAfterResultsChange()
    }
}
