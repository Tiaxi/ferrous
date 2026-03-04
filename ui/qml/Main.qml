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
    readonly property bool visualFeedsEnabled: visible
        && visibility !== Window.Minimized
        && active
    readonly property var uiBridge: bridge ? bridge : bridgeFallback

    QtObject {
        id: bridgeFallback
        property string playbackState: "Stopped"
        property string positionText: "00:00"
        property string durationText: "00:00"
        property real positionSeconds: 0
        property real durationSeconds: 0
        property real volume: 1.0
        property int queueLength: 0
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
        property var libraryTree: []
        property int libraryVersion: 0
        property bool libraryScanInProgress: false
        property int libraryRootCount: 0
        property int libraryTrackCount: 0
        property var libraryRoots: []
        property int librarySortMode: 0
        property string fileBrowserName: "File Manager"
        property int libraryScanRootsCompleted: 0
        property int libraryScanRootsTotal: 0
        property int libraryScanDiscovered: 0
        property int libraryScanProcessed: 0
        property real libraryScanFilesPerSecond: 0
        property real libraryScanEtaSeconds: -1
        property bool connected: false
        signal snapshotChanged()
        signal analysisChanged()
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
        function setLibrarySortMode(mode) {}
        function openInFileBrowser(path) {}
        function openContainingFolder(path) {}
        function scanRoot(path) {}
        function scanDefaultMusicRoot() {}
        function requestSnapshot() {}
        function shutdown() {}
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
        const paths = rowMap.playPaths || []
        return paths.length > 0
    }

    function appendLibraryRow(rowMap) {
        if (!isActionableLibraryRow(rowMap)) {
            return false
        }
        uiBridge.appendPaths(rowMap.playPaths || [])
        return true
    }

    function replaceWithLibraryRow(rowMap) {
        if (!isActionableLibraryRow(rowMap)) {
            return false
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
        if (!libraryAlbumView) {
            libraryModel.toggleKey(key)
            return
        }
        const preserveY = libraryAlbumView.contentY
        const restoreY = function() {
            const maxYNow = Math.max(0, libraryAlbumView.contentHeight - libraryAlbumView.height)
            libraryAlbumView.contentY = Math.min(preserveY, maxYNow)
        }
        libraryModel.toggleKey(key)
        restoreY()
        Qt.callLater(restoreY)
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

    function focusLibrarySearch() {
        librarySearchField.forceActiveFocus()
        librarySearchField.selectAll()
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
        text: "Append Library Selection"
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
        id: focusSearchAction
        text: "Focus Search"
        shortcut: StandardKey.Find
        onTriggered: root.focusLibrarySearch()
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
                { label: focusSearchAction.text, shortcut: String(focusSearchAction.shortcut) },
                { label: refreshSnapshotAction.text, shortcut: String(refreshSnapshotAction.shortcut) },
                { label: autoCenterSelectionAction.text, shortcut: "" },
                { label: resetSpectrogramAction.text, shortcut: "" },
                { label: showFpsOverlayAction.text, shortcut: "" }
            ])
            MenuItem { action: focusSearchAction }
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

                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: "Sort:" }
                                ComboBox {
                                    id: librarySortModeCombo
                                    model: ["Year", "Title"]
                                    Layout.preferredWidth: 120
                                    currentIndex: Math.max(0, Math.min(1, uiBridge.librarySortMode))
                                    onActivated: uiBridge.setLibrarySortMode(currentIndex)
                                }
                                ToolButton {
                                    icon.name: "document-edit"
                                    display: AbstractButton.IconOnly
                                    onClicked: scanFolderAction.trigger()
                                    ToolTip.visible: hovered
                                    ToolTip.text: "Add root"
                                }
                                Button {
                                    text: "Rescan All"
                                    onClicked: uiBridge.rescanAllLibraryRoots()
                                }
                            }

                            TextField {
                                id: librarySearchField
                                Layout.fillWidth: true
                                placeholderText: "Search"
                                onTextChanged: {
                                    libraryModel.setSearchText(text)
                                    root.syncLibrarySelectionToVisibleRows()
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.min(140, 28 * Math.max(1, uiBridge.libraryRoots.length))
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

                            Label {
                                Layout.fillWidth: true
                                readonly property int scanBacklog: Math.max(
                                    0,
                                    uiBridge.libraryScanDiscovered - uiBridge.libraryScanProcessed)
                                text: "Indexed tracks: " + uiBridge.libraryTrackCount
                                      + " | roots: " + uiBridge.libraryRootCount
                                      + (uiBridge.libraryScanInProgress
                                          ? (" | scanning " + uiBridge.libraryScanProcessed
                                             + "/" + Math.max(uiBridge.libraryScanDiscovered, uiBridge.libraryScanProcessed)
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
                                reuseItems: true
                                cacheBuffer: 480
                                boundsBehavior: Flickable.StopAtBounds
                                flickDeceleration: 2600
                                maximumFlickVelocity: 5200
                                ScrollBar.vertical: ScrollBar {
                                    policy: ScrollBar.AlwaysOn
                                }

                                delegate: Rectangle {
                                    id: libraryRow
                                    readonly property string rowTypeResolved: rowType || ""
                                    readonly property bool isAlbumRow: rowTypeResolved === "album"
                                    readonly property bool isTrackRow: rowTypeResolved === "track"
                                    readonly property bool hasChildren: !isTrackRow && (key || "").length > 0
                                    readonly property string selectionKeyResolved: selectionKey || ""
                                    readonly property string trackPathResolved: trackPath || ""
                                    readonly property string openPathResolved: openPath || ""
                                    readonly property var playPathsResolved: playPaths || []
                                    readonly property bool draggableLibraryItem: playPathsResolved.length > 0
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
                                            text: "Append"
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
                                text: uiBridge.libraryTree.length === 0
                                    ? (uiBridge.libraryScanInProgress ? "Scanning library..." : "No library rows indexed")
                                    : "No results"
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
            if (uiBridge.libraryVersion !== root.lastAppliedLibraryVersion) {
                const preserveY = libraryAlbumView ? libraryAlbumView.contentY : 0
                libraryModel.setLibraryTree(uiBridge.libraryTree || [])
                root.lastAppliedLibraryVersion = uiBridge.libraryVersion
                root.syncLibrarySelectionToVisibleRows()
                if (libraryAlbumView) {
                    const maxYNow = Math.max(0, libraryAlbumView.contentHeight - libraryAlbumView.height)
                    libraryAlbumView.contentY = Math.min(preserveY, maxYNow)
                }
            }
            root.syncQueueSelectionToCurrentQueue()
        }
        function onAnalysisChanged() {
            applyAnalysisDelta()
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

    Component.onCompleted: {
        libraryModel.setLibraryTree(uiBridge.libraryTree || [])
        libraryModel.setSearchText(librarySearchField.text || "")
        root.lastAppliedLibraryVersion = uiBridge.libraryVersion
        root.displayedPositionSeconds = uiBridge.positionSeconds
        root.positionSmoothingPrimed = uiBridge.playbackState === "Playing"
        root.positionSmoothingAnchorSeconds = uiBridge.positionSeconds
        root.positionSmoothingLastMs = Date.now()
        root.positionSmoothingTrackPath = uiBridge.currentTrackPath
        root.syncQueueSelectionToCurrentQueue()
        root.syncLibrarySelectionToVisibleRows()
    }
}
