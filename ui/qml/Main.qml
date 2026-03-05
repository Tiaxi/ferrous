import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import QtQml 2.15
import Qt.labs.platform 1.1 as Platform
import FerrousNative 1.0
import org.kde.kirigami 2.20 as Kirigami

Kirigami.ApplicationWindow {
    id: root
    width: 1600
    height: 980
    minimumWidth: 1280
    minimumHeight: 780
    visible: true
    title: "Ferrous"
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
    property int queueSelectionAnchorIndex: -1
    property int lastAppliedLibraryVersion: -1
    property int pendingLibraryVersion: -1
    property bool hasReceivedLibraryTreeFrame: false
    property string pendingLibraryAnchorKey: ""
    property real pendingLibraryAnchorOffset: 0
    property real pendingLibraryAnchorFallbackY: 0
    property bool pendingLibraryAnchorValid: false
    property int lastSeenQueueVersion: -1
    property int lastCenteredQueueIndex: -2
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
    property string transientBridgeError: ""
    property string libraryTypeAheadBuffer: ""
    property string pendingLibraryRevealSelectionKey: ""
    property var pendingLibraryRevealExpandKeys: []
    property int pendingLibraryRevealAttempts: 0
    property string pendingSearchOpenSelectionKey: ""
    property var pendingSearchOpenExpandKeys: []
    property int pendingSearchOpenAttempts: 0
    property int globalSearchSelectedDisplayIndex: -1
    property var globalSearchContextRowData: ({})
    property bool globalSearchOpening: false
    property bool globalSearchIgnoreRefocusFind: false
    property string pendingGlobalSearchPrefillText: ""
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: function(event) {
        root.tryCaptureGlobalSearchPrefill(event)
    }
    readonly property bool visualFeedsEnabled: visible
        && visibility !== Window.Minimized
        && active
    readonly property var uiBridge: bridge ? bridge : bridgeFallback
    readonly property var globalSearchModelApi: (uiBridge
        && uiBridge.globalSearchModel
        && uiBridge.globalSearchModel.nextSelectableIndex)
        ? uiBridge.globalSearchModel
        : globalSearchModelFallback

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
        property var queueItems: []
        property int selectedQueueIndex: -1
        property int playingQueueIndex: -1
        property string currentTrackPath: ""
        property string currentTrackCoverPath: ""
        property var waveformPeaksPacked: ""
        property bool spectrogramReset: false
        property real dbRange: 90
        property bool logScale: false
        property int repeatMode: 0
        property bool shuffleEnabled: false
        property bool showFps: false
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
        function replaceWithPaths(paths) {}
        function appendPaths(paths) {}
        function libraryAlbumCoverAt(index) { return "" }
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

    FontMetrics {
        id: menuFontMetrics
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
                uiBridge.appendAlbumByKey(rowMap.artist || "", rowMap.name || "")
            }
            return true
        }
        if (rowType === "artist") {
            uiBridge.appendArtistByName(rowMap.artist || "")
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
                uiBridge.replaceAlbumByKey(rowMap.artist || "", rowMap.name || "")
            }
            return true
        }
        if (rowType === "artist") {
            uiBridge.replaceArtistByName(rowMap.artist || "")
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
        return selectedQueueIndices.indexOf(index) >= 0
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
        pendingLibraryAnchorValid = false
        libraryModel.toggleKey(key)
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

    function finishPendingLibraryTreeApply() {
        if (pendingLibraryVersion < 0 || libraryModel.parsing) {
            return
        }
        lastAppliedLibraryVersion = pendingLibraryVersion
        pendingLibraryVersion = -1
        root.syncLibrarySelectionToVisibleRows()
        if (pendingLibraryAnchorValid) {
            restoreLibraryViewAnchor({
                key: pendingLibraryAnchorKey,
                offset: pendingLibraryAnchorOffset,
                fallbackY: pendingLibraryAnchorFallbackY
            })
            pendingLibraryAnchorValid = false
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
            selectedQueueIndices = [uiBridge.selectedQueueIndex]
            queueSelectionAnchorIndex = uiBridge.selectedQueueIndex
        } else {
            selectedQueueIndices = []
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
        selectedQueueIndices = []
        queueSelectionAnchorIndex = -1
        uiBridge.selectQueueIndex(-1)
    }

    function setQueueSingleSelection(index) {
        if (index < 0 || index >= uiBridge.queueLength) {
            clearQueueSelection()
            return
        }
        selectedQueueIndices = [index]
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
        selectedQueueIndices = indices
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
        selectedQueueIndices = indices
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
        for (let i = 0; i < selectedQueueIndices.length; ++i) {
            const idx = selectedQueueIndices[i]
            if (idx >= 0 && idx < uiBridge.queueLength && valid.indexOf(idx) < 0) {
                valid.push(idx)
            }
        }
        valid.sort(function(a, b) { return a - b })
        if (valid.length === 0 && uiBridge.selectedQueueIndex >= 0) {
            valid.push(uiBridge.selectedQueueIndex)
        }
        selectedQueueIndices = valid
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

    function statusLineText() {
        if (root.transientBridgeError.length > 0) {
            return "error | " + root.transientBridgeError
        }
        if (!uiBridge.connected) {
            return "bridge disconnected"
        }
        return uiBridge.playbackState.toLowerCase()
            + " | " + uiBridge.positionText + "/" + uiBridge.durationText
            + " | tracks " + uiBridge.queueLength
            + " | qdur " + uiBridge.queueDurationText
            + " | sel q:" + queueSelectionCount() + " l:" + librarySelectionCount()
            + " | " + librarySelectionStatusText()
            + " | " + repeatModeText(uiBridge.repeatMode)
            + " | " + (uiBridge.shuffleEnabled ? "shuffle-on" : "shuffle-off")
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

    function removeSelectedQueueTrack() {
        if (selectedQueueIndices.length > 0) {
            const indices = selectedQueueIndices.slice()
            indices.sort(function(a, b) { return b - a })
            for (let i = 0; i < indices.length; ++i) {
                uiBridge.removeAt(indices[i])
            }
            selectedQueueIndices = []
            queueSelectionAnchorIndex = -1
            return
        }
        if (uiBridge.selectedQueueIndex >= 0) {
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
                let headerIndex = index
                while (headerIndex > 0) {
                    const candidate = globalSearchModelApi.rowDataAt(headerIndex - 1)
                    if (!candidate || (candidate.kind || "") === "item") {
                        break
                    }
                    headerIndex -= 1
                }
                globalSearchResultsView.positionViewAtIndex(headerIndex, ListView.Beginning)
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
        root.globalSearchOpening = true
        root.globalSearchIgnoreRefocusFind = true
        root.pendingGlobalSearchPrefillText = ""
        globalSearchDialog.open()
        Qt.callLater(function() {
            if (globalSearchDialog.visible) {
                root.focusGlobalSearchQueryField(false)
            }
        })
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
        if (openingText.length !== 1
                || openingText === "\n"
                || openingText === "\r"
                || openingText === "\t") {
            return false
        }
        root.pendingGlobalSearchPrefillText += openingText
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
            uiBridge.playTrack(row.trackPath || "")
        } else if (rowType === "album") {
            const albumName = (row.album || row.label || "").trim()
            uiBridge.replaceAlbumByKey((row.artist || "").trim(), albumName)
        } else if (rowType === "artist") {
            uiBridge.replaceArtistByName((row.artist || row.label || "").trim())
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
            uiBridge.appendAlbumByKey((row.artist || "").trim(), albumName)
            return
        }
        if (rowType === "artist") {
            uiBridge.appendArtistByName((row.artist || row.label || "").trim())
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
        albumArtViewer.open()
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
        id: scanMusicAction
        text: "Scan Music Folder"
        shortcut: "Ctrl+R"
        onTriggered: uiBridge.scanDefaultMusicRoot()
    }
    Action {
        id: scanFolderAction
        text: "Scan Folder..."
        onTriggered: root.promptAddLibraryRoot("file_menu")
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
        id: refreshSnapshotAction
        text: "Refresh Snapshot"
        shortcut: "F5"
        onTriggered: uiBridge.requestSnapshot()
    }
    Action {
        id: autoCenterSelectionAction
        text: "Auto-center Queue Selection"
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
        shortcut: "Media Play"
        onTriggered: uiBridge.play()
    }
    Action {
        id: pauseAction
        text: "Pause"
        icon.name: "media-playback-pause"
        shortcut: "Media Pause"
        onTriggered: uiBridge.pause()
    }
    Action {
        id: stopAction
        text: "Stop"
        icon.name: "media-playback-stop"
        shortcut: "Media Stop"
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
    Shortcut {
        sequence: "Media Previous"
        onActivated: previousAction.trigger()
    }
    Shortcut {
        sequence: "Media Next"
        onActivated: nextAction.trigger()
    }
    menuBar: MenuBar {
        Menu {
            title: "File"
            width: root.menuPopupWidth([
                { label: playLibrarySelectionAction.text, shortcut: "" },
                { label: appendLibrarySelectionAction.text, shortcut: "" },
                { label: scanMusicAction.text, shortcut: String(scanMusicAction.shortcut) },
                { label: scanFolderAction.text, shortcut: "" },
                { label: refreshSnapshotAction.text, shortcut: String(refreshSnapshotAction.shortcut) },
                { label: quitAction.text, shortcut: String(quitAction.shortcut) }
            ])
            MenuItem { action: playLibrarySelectionAction }
            MenuItem { action: appendLibrarySelectionAction }
            MenuSeparator {}
            MenuItem { action: scanMusicAction }
            MenuItem { action: scanFolderAction }
            MenuItem { action: refreshSnapshotAction }
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
                { label: diagnosticsAction.text, shortcut: "" },
                { label: refreshSnapshotAction.text, shortcut: String(refreshSnapshotAction.shortcut) },
                { label: autoCenterSelectionAction.text, shortcut: "" },
                { label: resetSpectrogramAction.text, shortcut: "" },
                { label: showFpsOverlayAction.text, shortcut: "" }
            ])
            MenuItem { action: globalSearchAction }
            MenuItem { action: diagnosticsAction }
            MenuItem { action: refreshSnapshotAction }
            MenuSeparator {}
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
                { label: moveTrackUpAction.text, shortcut: String(moveTrackUpAction.shortcut) },
                { label: moveTrackDownAction.text, shortcut: String(moveTrackDownAction.shortcut) },
                { label: clearPlaylistAction.text, shortcut: "" }
            ])
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
            MenuItem { action: moveTrackUpAction }
            MenuItem { action: moveTrackDownAction }
            MenuSeparator {}
            MenuItem { action: clearPlaylistAction }
        }
        Menu {
            title: "Help"
            width: root.menuPopupWidth([
                { label: aboutAction.text, shortcut: "" }
            ])
            MenuItem { action: aboutAction }
        }
    }

    Dialog {
        id: aboutDialog
        modal: true
        title: "About Ferrous"
        standardButtons: Dialog.Ok
        width: 420
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

        contentItem: ScrollView {
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: preferencesDialog.availableWidth
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
                            Button {
                                text: "Add ~/Music"
                                onClicked: uiBridge.scanDefaultMusicRoot()
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
                            checked: uiBridge.logScale
                            onToggled: uiBridge.setLogScale(checked)
                        }
                        CheckBox {
                            text: "Show Spectrogram FPS Overlay"
                            checked: uiBridge.showFps
                            onToggled: uiBridge.setShowFps(checked)
                        }
                    }
                }

                GroupBox {
                    title: "Interface"
                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        CheckBox {
                            text: "Auto-center Queue Selection"
                            checked: root.autoCenterQueueSelection
                            onToggled: root.autoCenterQueueSelection = checked
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
        onOpened: {
            root.globalSearchOpening = false
            root.globalSearchIgnoreRefocusFind = true
            globalSearchOpenSettleTimer.restart()
            root.syncGlobalSearchSelectionAfterResultsChange()
            root.focusGlobalSearchQueryField(false)
            if ((root.pendingGlobalSearchPrefillText || "").length > 0) {
                globalSearchQueryField.text = root.pendingGlobalSearchPrefillText
                root.pendingGlobalSearchPrefillText = ""
            } else if ((globalSearchQueryField.text || "").length > 0) {
                globalSearchQueryField.cursorPosition = (globalSearchQueryField.text || "").length
            }
            uiBridge.setGlobalSearchQuery(globalSearchQueryField.text || "")
        }
        onClosed: {
            root.globalSearchOpening = false
            root.globalSearchIgnoreRefocusFind = false
            globalSearchOpenSettleTimer.stop()
            root.pendingGlobalSearchPrefillText = ""
            uiBridge.setGlobalSearchQuery("")
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
                color: Kirigami.Theme.disabledTextColor
                text: "Artists: " + (uiBridge.globalSearchArtistCount || 0)
                    + " | Albums: " + (uiBridge.globalSearchAlbumCount || 0)
                    + " | Tracks: " + (uiBridge.globalSearchTrackCount || 0)
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Qt.rgba(0, 0, 0, 0.02)
                border.color: Qt.rgba(0, 0, 0, 0.08)

                ListView {
                    id: globalSearchResultsView
                    anchors.fill: parent
                    clip: true
                    model: uiBridge.globalSearchModel || []
                    reuseItems: true
                    spacing: 0
                    boundsBehavior: Flickable.StopAtBounds
                    readonly property int reservedRightPadding: (globalSearchResultsScrollBar.visible
                        ? globalSearchResultsScrollBar.width + 8
                        : 8)
                    ScrollBar.vertical: ScrollBar {
                        id: globalSearchResultsScrollBar
                        policy: ScrollBar.AsNeeded
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
                            ? Kirigami.Theme.highlightedTextColor
                            : Kirigami.Theme.textColor
                        width: Math.max(
                            0,
                            ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                        height: rowKind === "section" ? 30 : 24
                        color: rowKind === "section"
                            ? Kirigami.Theme.alternateBackgroundColor
                            : (rowKind === "columns"
                                ? Qt.rgba(0, 0, 0, 0.05)
                                : (index === root.globalSearchSelectedDisplayIndex
                                    ? Kirigami.Theme.highlightColor
                                    : (index % 2 === 0
                                        ? Kirigami.Theme.backgroundColor
                                        : Kirigami.Theme.alternateBackgroundColor)))

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8

                            Label {
                                visible: rowKind === "section"
                                Layout.fillWidth: true
                                text: sectionTitleValue || ""
                                font.bold: true
                            }

                            RowLayout {
                                visible: rowKind === "columns" && rowTypeValue === "artist"
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: "Name"; Layout.fillWidth: true; font.bold: true }
                            }

                            RowLayout {
                                visible: rowKind === "columns" && rowTypeValue === "album"
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: ""; Layout.preferredWidth: 26; font.bold: true }
                                Label { text: "Title"; Layout.fillWidth: true; font.bold: true }
                                Label { text: "Artist"; Layout.preferredWidth: 170; font.bold: true }
                                Label { text: "Year"; Layout.preferredWidth: 52; font.bold: true }
                                Label { text: "Genre"; Layout.preferredWidth: 120; font.bold: true }
                                Label { text: "#"; Layout.preferredWidth: 34; font.bold: true; horizontalAlignment: Text.AlignRight }
                                Label { text: "Length"; Layout.preferredWidth: 76; font.bold: true; horizontalAlignment: Text.AlignRight }
                            }

                            RowLayout {
                                visible: rowKind === "columns" && rowTypeValue === "track"
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: "#"; Layout.preferredWidth: 34; font.bold: true }
                                Label { text: "Title"; Layout.fillWidth: true; font.bold: true }
                                Label { text: "Artist"; Layout.preferredWidth: 160; font.bold: true }
                                Label { text: ""; Layout.preferredWidth: 20; font.bold: true }
                                Label { text: "Album"; Layout.preferredWidth: 182; font.bold: true }
                                Label {
                                    text: "Year"
                                    Layout.preferredWidth: 52
                                    font.bold: true
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label { text: "Genre"; Layout.preferredWidth: 112; font.bold: true }
                                Label { text: "Length"; Layout.preferredWidth: 76; font.bold: true; horizontalAlignment: Text.AlignRight }
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
                                    Layout.preferredWidth: 76
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
                                    Layout.preferredWidth: 112
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: lengthTextValue || "--:--"
                                    Layout.preferredWidth: 76
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

    Platform.FolderDialog {
        id: scanFolderDialog
        title: "Select Music Folder to Scan"
        onAccepted: {
            const localPath = root.folderDialogPath(scanFolderDialog)
            if (localPath.length > 0) {
                uiBridge.addLibraryRoot(localPath)
            }
            pendingFolderDialogContext = ""
        }
        onRejected: pendingFolderDialogContext = ""
    }

    footer: ToolBar {
        contentItem: RowLayout {
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: statusLineText()
                elide: Text.ElideRight
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            id: transportBar
            Layout.fillWidth: true
            implicitHeight: 56

            contentItem: RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 12
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
                    readonly property real stableVisualPosition: durationKnown ? visualPosition : 0.0
                    stepSize: 0
                    onPressedChanged: {
                        if (!pressed) {
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
                            peaksData: uiBridge.waveformPeaksPacked
                            positionSeconds: root.displayedPositionSeconds
                            durationSeconds: uiBridge.durationSeconds
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: Math.round(parent.width * seekSlider.stableVisualPosition)
                            color: Qt.rgba(120 / 255, 190 / 255, 1.0, 0.26)
                        }

                        Rectangle {
                            width: 1
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            x: Math.round(seekSlider.stableVisualPosition * (parent.width - 1))
                            color: "#2f7cd6"
                        }
                    }

                    handle: Rectangle {
                        x: seekSlider.leftPadding + seekSlider.stableVisualPosition * (seekSlider.availableWidth - width)
                        y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                        implicitWidth: 3
                        implicitHeight: seekSlider.height - 4
                        radius: 1
                        color: "#2f7cd6"
                        border.color: "#1f5aa7"
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
                    horizontalAlignment: Text.AlignRight
                    Layout.minimumWidth: 96
                }

                ToolButton {
                    icon.name: "audio-volume-high"
                    display: AbstractButton.IconOnly
                    enabled: false
                    focusPolicy: Qt.NoFocus
                }

                Slider {
                    id: volumeSlider
                    Layout.preferredWidth: 140
                    from: 0
                    to: 1
                    stepSize: 0
                    onMoved: uiBridge.setVolume(value)
                    onPressedChanged: {
                        if (!pressed) {
                            uiBridge.setVolume(value)
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
                color: Kirigami.Theme.backgroundColor
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
                        color: Kirigami.Theme.backgroundColor
                        border.color: Qt.rgba(0, 0, 0, 0.12)

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 6

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
                                flickDeceleration: 2600
                                maximumFlickVelocity: 5200
                                ScrollBar.vertical: ScrollBar {
                                    policy: ScrollBar.AlwaysOn
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
                                        ? Kirigami.Theme.highlightColor
                                        : (index % 2 === 0
                                            ? Kirigami.Theme.backgroundColor
                                            : Kirigami.Theme.alternateBackgroundColor)

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
                                                ? Kirigami.Theme.highlightedTextColor
                                                : Kirigami.Theme.disabledTextColor
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
                                                ? Kirigami.Theme.highlightedTextColor
                                                : Kirigami.Theme.textColor
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
                                color: Kirigami.Theme.disabledTextColor
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
                    color: Kirigami.Theme.backgroundColor
                    SplitView.fillWidth: true
                    SplitView.preferredHeight: root.height * 0.58
                    SplitView.minimumHeight: 220
                    border.color: Qt.rgba(0, 0, 0, 0.12)

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 26
                            color: Kirigami.Theme.alternateBackgroundColor
                            border.color: Qt.rgba(0, 0, 0, 0.08)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                Label { text: "#"; Layout.preferredWidth: 24 }
                                Label { text: "Title"; Layout.fillWidth: true }
                                Label { text: "Length"; Layout.preferredWidth: 72 }
                            }
                        }

                        ListView {
                            id: playlistView
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: uiBridge.queueItems

                            delegate: Rectangle {
                                width: ListView.view.width
                                height: 24
                                color: root.isQueueIndexSelected(index)
                                    ? Kirigami.Theme.highlightColor
                                    : (index % 2 === 0 ? Kirigami.Theme.backgroundColor
                                                        : Kirigami.Theme.alternateBackgroundColor)

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    Label {
                                        text: (uiBridge.playbackState !== "Stopped"
                                                && index === uiBridge.playingQueueIndex)
                                            ? "▶"
                                            : (index + 1).toString().padStart(2, "0")
                                        Layout.preferredWidth: 24
                                        font.bold: uiBridge.playbackState !== "Stopped"
                                            && index === uiBridge.playingQueueIndex
                                        color: root.isQueueIndexSelected(index)
                                            ? Kirigami.Theme.highlightedTextColor
                                            : ((uiBridge.playbackState !== "Stopped"
                                                && index === uiBridge.playingQueueIndex)
                                                ? Kirigami.Theme.positiveTextColor
                                                : Kirigami.Theme.textColor)
                                    }
                                    Label {
                                        text: modelData
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        color: root.isQueueIndexSelected(index)
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }
                                    Label {
                                        text: ""
                                        Layout.preferredWidth: 72
                                        horizontalAlignment: Text.AlignRight
                                        color: root.isQueueIndexSelected(index)
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onPressed: function(mouse) {
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
                            }

                            Menu {
                                id: playlistContextMenu
                                property int rowIndex: -1

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
                                if (uiBridge.selectedQueueIndex >= 0
                                        && uiBridge.selectedQueueIndex !== root.lastCenteredQueueIndex
                                        && root.autoCenterQueueSelection) {
                                    playlistView.positionViewAtIndex(uiBridge.selectedQueueIndex, ListView.Contain)
                                    root.lastCenteredQueueIndex = uiBridge.selectedQueueIndex
                                } else if (uiBridge.selectedQueueIndex < 0) {
                                    root.lastCenteredQueueIndex = -2
                                }
                            }
                        }
                    }

                    DropArea {
                        id: playlistDropArea
                        anchors.fill: parent

                        onDropped: function(drop) {
                            const src = drop.source
                            if (!src || !src.draggableLibraryItem) {
                                return
                            }
                            const rowMap = {
                                selectionKey: src.selectionKeyResolved || "",
                                sourceIndex: src.sourceIndexResolved !== undefined ? src.sourceIndexResolved : -1,
                                rowType: src.rowTypeResolved || "",
                                artist: src.artist || "",
                                name: src.name || "",
                                title: src.rowTitle || "",
                                trackPath: src.trackPathResolved || "",
                                openPath: src.openPathResolved || "",
                                playPaths: src.playPathsResolved || []
                            }
                            const rows = root.rowsForLibraryAction(rowMap)
                            if (rows.length > 0) {
                                root.appendLibraryRows(rows)
                                drop.acceptProposedAction()
                            }
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: playlistDropArea.containsDrag ? 2 : 0
                        border.color: Kirigami.Theme.highlightColor
                        visible: playlistDropArea.containsDrag
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
                property: "opacity"
                from: 0.0
                to: 1.0
                duration: 140
                easing.type: Easing.OutCubic
            }
        }
        exit: Transition {
            NumberAnimation {
                property: "opacity"
                from: 1.0
                to: 0.0
                duration: 120
                easing.type: Easing.InCubic
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
        onOpened: {
            root.albumArtZoom = 1.0
            root.albumArtPanX = 0.0
            root.albumArtPanY = 0.0
        }

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
            anchors.margins: 20
            clip: true

            Item {
                id: albumArtTransform
                width: albumArtViewport.width * 0.92
                height: albumArtViewport.height * 0.92
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
            }
            root.syncQueueSelectionToCurrentQueue()
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
        root.displayedPositionSeconds = uiBridge.positionSeconds
        root.positionSmoothingPrimed = uiBridge.playbackState === "Playing"
        root.positionSmoothingAnchorSeconds = uiBridge.positionSeconds
        root.positionSmoothingLastMs = Date.now()
        root.positionSmoothingTrackPath = uiBridge.currentTrackPath
        root.syncQueueSelectionToCurrentQueue()
        root.syncLibrarySelectionToVisibleRows()
        root.syncGlobalSearchSelectionAfterResultsChange()
    }
}
