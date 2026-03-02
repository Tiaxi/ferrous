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
    property var selectedLibrarySelectionKeys: []
    property int librarySelectionAnchorIndex: -1
    property var selectedQueueIndices: []
    property int queueSelectionAnchorIndex: -1
    property int lastAppliedLibraryVersion: -1
    property int lastCenteredQueueIndex: -2
    property bool autoCenterQueueSelection: true
    property real displayedPositionSeconds: 0
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
        function libraryAlbumCoverAt(index) { return "" }
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
        running: !seekSlider.pressed && uiBridge.playbackState === "Playing" && root.visualFeedsEnabled
        onTriggered: {
            const duration = Math.max(uiBridge.durationSeconds, 0)
            if (duration <= 0) {
                root.displayedPositionSeconds = 0
                return
            }
            root.displayedPositionSeconds = Math.min(
                duration,
                root.displayedPositionSeconds + interval / 1000.0)
        }
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

    function canPlayLibrarySelection() {
        if (selectedLibraryRowType === "artist") {
            return selectedLibraryArtist.length > 0
        }
        if (selectedLibraryRowType === "album") {
            return selectedLibrarySourceIndex >= 0
        }
        if (selectedLibraryRowType === "track") {
            return selectedLibraryTrackPath.length > 0
        }
        return false
    }

    function playLibrarySelection() {
        if (selectedLibraryRowType === "artist" && selectedLibraryArtist.length > 0) {
            uiBridge.replaceArtistByName(selectedLibraryArtist)
        } else if (selectedLibraryRowType === "album" && selectedLibrarySourceIndex >= 0) {
            uiBridge.replaceAlbumAt(selectedLibrarySourceIndex)
        } else if (selectedLibraryRowType === "track" && selectedLibraryTrackPath.length > 0) {
            uiBridge.playTrack(selectedLibraryTrackPath)
        }
    }

    function appendLibrarySelection() {
        if (selectedLibraryRowType === "artist" && selectedLibraryArtist.length > 0) {
            uiBridge.appendArtistByName(selectedLibraryArtist)
        } else if (selectedLibraryRowType === "album" && selectedLibrarySourceIndex >= 0) {
            uiBridge.appendAlbumAt(selectedLibrarySourceIndex)
        } else if (selectedLibraryRowType === "track" && selectedLibraryTrackPath.length > 0) {
            uiBridge.appendTrack(selectedLibraryTrackPath)
        }
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
        if (selectedLibraryRowType === "artist" && selectedLibraryArtist.length > 0) {
            return "artist: " + selectedLibraryArtist
        }
        if (selectedLibraryRowType === "album" && selectedLibraryAlbum.length > 0) {
            return "album: " + selectedLibraryAlbum
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
    }

    function applyLibraryPrimaryRow(rowMap) {
        root.selectedLibrarySelectionKey = rowMap.selectionKey || ""
        root.selectedLibrarySourceIndex = rowMap.sourceIndex !== undefined ? rowMap.sourceIndex : -1
        root.selectedLibraryRowType = rowMap.rowType || ""
        root.selectedLibraryArtist = rowMap.artist || ""
        root.selectedLibraryAlbum = rowMap.name || ""
        root.selectedLibraryTrackPath = rowMap.trackPath || ""
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
        const value = String(urlValue || "")
        if (value.startsWith("file://")) {
            return decodeURIComponent(value.substring(7))
        }
        return value
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
        onTriggered: scanFolderDialog.open()
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
                { label: clearPlaylistAction.text, shortcut: "" }
            ])
            MenuItem { action: removeSelectedTrackAction }
            MenuItem { action: moveTrackUpAction }
            MenuItem { action: moveTrackDownAction }
            MenuSeparator {}
            MenuItem { action: selectPreviousTrackAction }
            MenuItem { action: selectNextTrackAction }
            MenuSeparator {}
            MenuItem { action: clearPlaylistAction }
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

    Platform.FolderDialog {
        id: scanFolderDialog
        title: "Select Music Folder to Scan"
        onAccepted: {
            const localPath = root.urlToLocalPath(folder)
            if (localPath.length > 0) {
                uiBridge.scanRoot(localPath)
            }
        }
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
                    stepSize: 0
                    onPressedChanged: {
                        if (!pressed) {
                            root.displayedPositionSeconds = value
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
                            width: Math.round(parent.width * seekSlider.visualPosition)
                            color: Qt.rgba(120 / 255, 190 / 255, 1.0, 0.26)
                        }

                        Rectangle {
                            width: 1
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            x: Math.round(seekSlider.visualPosition * (parent.width - 1))
                            color: "#2f7cd6"
                        }
                    }

                    handle: Rectangle {
                        x: seekSlider.leftPadding + seekSlider.visualPosition * (seekSlider.availableWidth - width)
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
                    value: root.displayedPositionSeconds
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
                                ComboBox {
                                    model: ["Folders"]
                                    Layout.fillWidth: true
                                }
                                ToolButton {
                                    icon.name: "document-edit"
                                    display: AbstractButton.IconOnly
                                    onClicked: scanFolderAction.trigger()
                                }
                                Button {
                                    text: "Scan Music"
                                    onClicked: uiBridge.scanDefaultMusicRoot()
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

                            Label {
                                Layout.fillWidth: true
                                text: "Indexed tracks: " + uiBridge.libraryTrackCount
                                      + " | roots: " + uiBridge.libraryRootCount
                                      + (uiBridge.libraryScanInProgress ? " | scanning..." : "")
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
                                    readonly property bool isArtistRow: rowType === "artist"
                                    readonly property bool isAlbumRow: rowType === "album"
                                    readonly property bool isTrackRow: rowType === "track"
                                    readonly property string artistName: artist !== undefined ? artist : ""
                                    readonly property string albumName: name !== undefined ? name : ""
                                    readonly property string trackPathResolved: trackPath !== undefined ? trackPath : ""
                                    readonly property bool draggableLibraryItem: isArtistRow
                                        || isAlbumRow
                                        || (isTrackRow && trackPathResolved.length > 0)
                                    readonly property int sourceIndexResolved: sourceIndex !== undefined ? sourceIndex : -1
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
                                            Layout.preferredWidth: isArtistRow ? 0 : (isAlbumRow ? 14 : 28)
                                        }

                                        Label {
                                            id: expanderIcon
                                            Layout.preferredWidth: 24
                                            Layout.fillHeight: true
                                            Layout.alignment: Qt.AlignVCenter
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                            text: (isArtistRow || isAlbumRow)
                                                ? (expanded ? "▾" : "▸")
                                                : ""
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
                                                source: coverPath || ""
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
                                            text: isArtistRow
                                                ? (artist + " (" + count + ")")
                                                : (isAlbumRow
                                                    ? (name + " (" + count + ")")
                                                    : (trackNumber.toString().padStart(2, "0")
                                                       + "  " + title))
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
                                            if (mouse.button === Qt.LeftButton
                                                    && (isArtistRow || isAlbumRow)
                                                    && mouse.x <= expanderIcon.x + expanderIcon.width + 6) {
                                                if (isArtistRow) {
                                                    libraryModel.toggleArtist(artist)
                                                } else {
                                                    libraryModel.toggleAlbum(key)
                                                }
                                                return
                                            }
                                            const rowMap = {
                                                selectionKey: selectionKey || "",
                                                sourceIndex: (isAlbumRow || isTrackRow) ? sourceIndexResolved : -1,
                                                rowType: rowType || "",
                                                artist: artist || "",
                                                name: name || "",
                                                trackPath: trackPath || ""
                                            }
                                            root.handleLibraryRowSelection(
                                                index,
                                                rowMap,
                                                mouse.button,
                                                mouse.modifiers || Qt.NoModifier)
                                            if (isArtistRow && mouse.button === Qt.RightButton) {
                                                artistMenu.popup()
                                                return
                                            }
                                            if (isAlbumRow) {
                                                if (mouse.button === Qt.RightButton) {
                                                    albumMenu.popup()
                                                }
                                            } else if (isTrackRow) {
                                                if (mouse.button === Qt.RightButton) {
                                                    trackMenu.popup()
                                                }
                                            }
                                        }
                                        onDoubleClicked: function(mouse) {
                                            if ((isArtistRow || isAlbumRow)
                                                    && mouse.x <= expanderIcon.x + expanderIcon.width + 6) {
                                                return
                                            }
                                            if (isArtistRow) {
                                                uiBridge.replaceArtistByName(artist)
                                            } else
                                            if (isAlbumRow && sourceIndexResolved >= 0) {
                                                uiBridge.replaceAlbumAt(sourceIndexResolved)
                                            } else if (isTrackRow && trackPath && trackPath.length > 0) {
                                                uiBridge.playTrack(trackPath)
                                            }
                                        }
                                    }

                                    Menu {
                                        id: albumMenu
                                        MenuItem {
                                            text: "Play Album"
                                            onTriggered: {
                                                if (sourceIndexResolved >= 0) {
                                                    uiBridge.replaceAlbumAt(sourceIndexResolved)
                                                }
                                            }
                                        }
                                        MenuItem {
                                            text: "Append Album"
                                            onTriggered: {
                                                if (sourceIndexResolved >= 0) {
                                                    uiBridge.appendAlbumAt(sourceIndexResolved)
                                                }
                                            }
                                        }
                                    }

                                    Menu {
                                        id: artistMenu
                                        MenuItem {
                                            text: "Play Artist"
                                            onTriggered: uiBridge.replaceArtistByName(artist)
                                        }
                                        MenuItem {
                                            text: "Append Artist"
                                            onTriggered: uiBridge.appendArtistByName(artist)
                                        }
                                    }

                                    Menu {
                                        id: trackMenu
                                        MenuItem {
                                            text: "Play Track"
                                            enabled: !!(trackPath && trackPath.length > 0)
                                            onTriggered: uiBridge.playTrack(trackPath)
                                        }
                                        MenuItem {
                                            text: "Append Track"
                                            enabled: !!(trackPath && trackPath.length > 0)
                                            onTriggered: uiBridge.appendTrack(trackPath)
                                        }
                                    }
                                }
                            }

                            Label {
                                visible: libraryAlbumView.count === 0
                                text: uiBridge.libraryAlbums.length === 0
                                    ? (uiBridge.libraryScanInProgress ? "Scanning library..." : "No albums indexed")
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
                                        text: index === uiBridge.playingQueueIndex
                                            ? "▶"
                                            : (index + 1).toString().padStart(2, "0")
                                        Layout.preferredWidth: 24
                                        font.bold: index === uiBridge.playingQueueIndex
                                        color: root.isQueueIndexSelected(index)
                                            ? Kirigami.Theme.highlightedTextColor
                                            : (index === uiBridge.playingQueueIndex
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
                            if (src.isArtistRow && src.artistName && src.artistName.length > 0) {
                                uiBridge.appendArtistByName(src.artistName)
                                drop.acceptProposedAction()
                                return
                            }
                            if (src.isAlbumRow && src.sourceIndexResolved >= 0) {
                                uiBridge.appendAlbumAt(src.sourceIndexResolved)
                                drop.acceptProposedAction()
                                return
                            }
                            if (src.isTrackRow
                                    && src.trackPathResolved
                                    && src.trackPathResolved.length > 0) {
                                uiBridge.appendTrack(src.trackPathResolved)
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
            if (uiBridge.playbackState !== "Playing"
                    || Math.abs(root.displayedPositionSeconds - incomingPosition) > 0.35) {
                root.displayedPositionSeconds = incomingPosition
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
            console.warn("bridge error:", message)
        }
    }

    Component.onCompleted: {
        libraryModel.setLibraryTree(uiBridge.libraryTree || [])
        libraryModel.setSearchText(librarySearchField.text || "")
        root.lastAppliedLibraryVersion = uiBridge.libraryVersion
        root.displayedPositionSeconds = uiBridge.positionSeconds
        root.syncQueueSelectionToCurrentQueue()
        root.syncLibrarySelectionToVisibleRows()
    }
}
