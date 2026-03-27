import QtQuick 2.15

QtObject {
    id: root

    required property var uiBridge
    required property var tagEditorApi
    required property var openTagEditorDialog

    property var view: null
    property var selectedIndices: []
    property var selectedIndexLookup: ({})
    property int selectionAnchorIndex: -1
    property int lastSyncedBridgeSelectedIndex: -2
    property int lastSeenQueueVersion: -1
    property int lastCenteredIndex: -2
    property string lastAutoCenterPlaybackState: ""
    property string lastAutoCenterTrackPath: ""
    property real viewportRestoreUntilMs: 0
    property real viewportRestoreContentY: 0
    property bool autoCenterSelection: true
    property int _pendingScrollIndex: -1
    property var _pendingScrollView: null

    property Timer _scrollTimer: Timer {
        interval: 0
        onTriggered: {
            const view = root._pendingScrollView
            const idx = root._pendingScrollIndex
            root._pendingScrollIndex = -1
            root._pendingScrollView = null
            if (!view || idx < 0) return
            if (root.uiBridge.profileLogsEnabled) {
                const t0 = Date.now()
                view.positionViewAtIndex(idx, ListView.Contain)
                const ms = Date.now() - t0
                if (ms >= 2)
                    console.warn("[qml-signal-profile] positionViewAtIndex(deferred) idx="
                        + idx + " queueLen=" + root.uiBridge.queueLength + " ms=" + ms)
            } else {
                view.positionViewAtIndex(idx, ListView.Contain)
            }
        }
    }

    function registerView(view) {
        root.view = view
    }

    function selectionCount() {
        if (root.selectedIndices.length > 0) {
            return root.selectedIndices.length
        }
        return root.uiBridge.selectedQueueIndex >= 0 ? 1 : 0
    }

    function isIndexSelected(index) {
        return !!root.selectedIndexLookup[index]
    }

    function setSelectedIndices(indices) {
        const next = indices || []
        root.selectedIndices = next
        const lookup = ({})
        for (let i = 0; i < next.length; ++i) {
            const idx = next[i]
            if (idx >= 0 && idx < root.uiBridge.queueLength) {
                lookup[idx] = true
            }
        }
        root.selectedIndexLookup = lookup
    }

    function resetSelectionForUpdatedQueue() {
        if (root.uiBridge.selectedQueueIndex >= 0
                && root.uiBridge.selectedQueueIndex < root.uiBridge.queueLength) {
            root.setSelectedIndices([root.uiBridge.selectedQueueIndex])
            root.selectionAnchorIndex = root.uiBridge.selectedQueueIndex
        } else {
            root.setSelectedIndices([])
            root.selectionAnchorIndex = -1
        }
    }

    function clearSelection() {
        root.setSelectedIndices([])
        root.selectionAnchorIndex = -1
        root.uiBridge.selectQueueIndex(-1)
    }

    function requestViewportRestoreWindow(durationMs) {
        if (!root.view) {
            return
        }
        const ms = Math.max(100, durationMs || 700)
        root.viewportRestoreContentY = root.view.contentY
        root.viewportRestoreUntilMs = Math.max(root.viewportRestoreUntilMs, Date.now() + ms)
    }

    function viewportRestoreActive() {
        return root.viewportRestoreUntilMs > Date.now()
    }

    function applyPendingViewportRestore() {
        if (!root.view || !root.viewportRestoreActive()) {
            return
        }
        const maxY = Math.max(0, root.view.contentHeight - root.view.height)
        const targetY = Math.max(0, Math.min(maxY, root.viewportRestoreContentY))
        if (Math.abs(root.view.contentY - targetY) > 0.5) {
            root.view.contentY = targetY
        }
    }

    function handleSnapshotChanged(view) {
        const playlistView = view || root.view
        if (!playlistView) {
            return
        }
        root.view = playlistView
        const playbackState = root.uiBridge.playbackState || ""
        const currentTrackPath = root.uiBridge.currentTrackPath || ""
        if (!root.autoCenterSelection) {
            root.lastAutoCenterPlaybackState = playbackState
            root.lastAutoCenterTrackPath = currentTrackPath
            return
        }
        if (root.viewportRestoreActive()) {
            root.lastAutoCenterPlaybackState = playbackState
            root.lastAutoCenterTrackPath = currentTrackPath
            return
        }
        const targetIndex = root.uiBridge.playingQueueIndex
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
        const needsInitialCenter = root.lastCenteredIndex < 0
        if (targetIndex >= 0 && (trackChanged || resumedFromStop || needsInitialCenter)) {
            if (playlistView.visible && playlistView.height > 0) {
                root._pendingScrollIndex = targetIndex
                root._pendingScrollView = playlistView
                root._scrollTimer.restart()
            }
            root.lastCenteredIndex = targetIndex
        }
        root.lastAutoCenterPlaybackState = playbackState
        root.lastAutoCenterTrackPath = currentTrackPath
    }

    function setSingleSelection(index) {
        if (index < 0 || index >= root.uiBridge.queueLength) {
            root.clearSelection()
            return
        }
        if (root.selectedIndices.length === 1
                && root.selectedIndices[0] === index
                && root.selectionAnchorIndex === index
                && root.uiBridge.selectedQueueIndex === index) {
            return
        }
        root.setSelectedIndices([index])
        root.selectionAnchorIndex = index
        root.uiBridge.selectQueueIndex(index)
    }

    function setRangeSelection(index) {
        if (index < 0 || index >= root.uiBridge.queueLength) {
            return
        }
        const anchor = root.selectionAnchorIndex >= 0
            ? root.selectionAnchorIndex
            : (root.uiBridge.selectedQueueIndex >= 0 ? root.uiBridge.selectedQueueIndex : index)
        const first = Math.min(anchor, index)
        const last = Math.max(anchor, index)
        const indices = []
        for (let i = first; i <= last; ++i) {
            indices.push(i)
        }
        root.setSelectedIndices(indices)
        root.selectionAnchorIndex = anchor
        root.uiBridge.selectQueueIndex(index)
    }

    function toggleSelection(index) {
        if (index < 0 || index >= root.uiBridge.queueLength) {
            return
        }
        const indices = root.selectedIndices.slice()
        const pos = indices.indexOf(index)
        if (pos >= 0) {
            indices.splice(pos, 1)
        } else {
            indices.push(index)
            indices.sort(function(a, b) { return a - b })
        }
        root.setSelectedIndices(indices)
        root.selectionAnchorIndex = index
        if (indices.length > 0) {
            root.uiBridge.selectQueueIndex(index)
        } else {
            root.uiBridge.selectQueueIndex(-1)
        }
    }

    function handleRowSelection(index, button, modifiers) {
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        if (shift) {
            root.setRangeSelection(index)
            return
        }
        if (ctrl) {
            root.toggleSelection(index)
            return
        }
        if (button === Qt.RightButton && root.isIndexSelected(index)) {
            root.selectionAnchorIndex = index
            root.uiBridge.selectQueueIndex(index)
            return
        }
        root.setSingleSelection(index)
    }

    function syncSelectionToCurrentQueue() {
        const valid = []
        const seen = ({})
        for (let i = 0; i < root.selectedIndices.length; ++i) {
            const idx = root.selectedIndices[i]
            if (idx >= 0 && idx < root.uiBridge.queueLength && !seen[idx]) {
                seen[idx] = true
                valid.push(idx)
            }
        }
        valid.sort(function(a, b) { return a - b })
        if (valid.length === 0 && root.uiBridge.selectedQueueIndex >= 0) {
            valid.push(root.uiBridge.selectedQueueIndex)
        }
        root.setSelectedIndices(valid)
        if (root.selectionAnchorIndex < 0 || root.selectionAnchorIndex >= root.uiBridge.queueLength) {
            root.selectionAnchorIndex = valid.length > 0 ? valid[valid.length - 1] : -1
        }
    }

    function selectRelative(delta) {
        if (root.uiBridge.queueLength <= 0) {
            return
        }
        const current = root.uiBridge.selectedQueueIndex >= 0
            ? root.uiBridge.selectedQueueIndex
            : root.uiBridge.playingQueueIndex
        const base = current >= 0 ? current : 0
        const nextIdx = Math.max(0, Math.min(root.uiBridge.queueLength - 1, base + delta))
        root.setSingleSelection(nextIdx)
    }

    function moveSelected(delta) {
        const from = root.uiBridge.selectedQueueIndex
        if (from < 0 || root.uiBridge.queueLength <= 0) {
            return
        }
        const to = Math.max(0, Math.min(root.uiBridge.queueLength - 1, from + delta))
        if (to !== from) {
            root.uiBridge.moveQueue(from, to)
        }
    }

    function openTagEditorForRow(rowIndex) {
        if (rowIndex < 0) {
            return
        }
        let indices = [rowIndex]
        if (root.isIndexSelected(rowIndex) && root.selectedIndices.length > 1) {
            indices = root.selectedIndices.slice().sort(function(a, b) { return a - b })
        }
        const selections = []
        for (let i = 0; i < indices.length; ++i) {
            const path = root.uiBridge.queuePathAt(indices[i])
            if (path && path.length > 0) {
                selections.push({ path: path })
            }
        }
        if (selections.length > 0 && root.tagEditorApi.openSelection(selections)) {
            root.openTagEditorDialog()
        }
    }

    function firstSelectedIndex() {
        let first = -1
        for (let i = 0; i < root.selectedIndices.length; ++i) {
            const idx = root.selectedIndices[i]
            if (idx < 0 || idx >= root.uiBridge.queueLength) {
                continue
            }
            if (first < 0 || idx < first) {
                first = idx
            }
        }
        if (first >= 0) {
            return first
        }
        if (root.uiBridge.selectedQueueIndex >= 0
                && root.uiBridge.selectedQueueIndex < root.uiBridge.queueLength) {
            return root.uiBridge.selectedQueueIndex
        }
        return -1
    }

    function playFirstSelectedTrack() {
        const target = root.firstSelectedIndex()
        if (target >= 0) {
            root.uiBridge.playAt(target)
        }
    }

    function pageStep() {
        const rowHeight = 24
        const viewportHeight = root.view ? root.view.height : 240
        return Math.max(1, Math.floor(viewportHeight / rowHeight) - 1)
    }

    function ensureIndexVisible(index) {
        if (!root.view || index < 0) {
            return
        }
        const firstVisible = root.view.indexAt(0, 0)
        const lastVisible = root.view.indexAt(0, root.view.height - 1)
        if (firstVisible >= 0
                && lastVisible >= 0
                && index >= firstVisible
                && index <= lastVisible) {
            return
        }
        root.view.positionViewAtIndex(index, ListView.Contain)
    }

    function selectAllItems() {
        if (root.uiBridge.queueLength <= 0) {
            root.clearSelection()
            return
        }
        const indices = []
        for (let i = 0; i < root.uiBridge.queueLength; ++i) {
            indices.push(i)
        }
        const primary = root.uiBridge.selectedQueueIndex >= 0
            ? root.uiBridge.selectedQueueIndex
            : 0
        root.setSelectedIndices(indices)
        root.selectionAnchorIndex = primary
        root.uiBridge.selectQueueIndex(primary)
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
            root.playFirstSelectedTrack()
            event.accepted = true
            return
        }

        if (ctrl && !shift && event.key === Qt.Key_A) {
            root.selectAllItems()
            event.accepted = true
            return
        }

        let delta = 0
        if (event.key === Qt.Key_Up) {
            delta = -1
        } else if (event.key === Qt.Key_Down) {
            delta = 1
        } else if (event.key === Qt.Key_PageUp) {
            delta = -root.pageStep()
        } else if (event.key === Qt.Key_PageDown) {
            delta = root.pageStep()
        } else {
            return
        }

        if (root.uiBridge.queueLength <= 0) {
            event.accepted = true
            return
        }

        const current = root.uiBridge.selectedQueueIndex >= 0
            ? root.uiBridge.selectedQueueIndex
            : (root.uiBridge.playingQueueIndex >= 0 ? root.uiBridge.playingQueueIndex : 0)
        const next = Math.max(0, Math.min(root.uiBridge.queueLength - 1, current + delta))
        if (shift) {
            root.setRangeSelection(next)
        } else {
            root.setSingleSelection(next)
        }
        root.ensureIndexVisible(next)
        event.accepted = true
    }

    function removeSelectedTrack() {
        if (root.selectedIndices.length > 0) {
            const indices = root.selectedIndices.slice()
            indices.sort(function(a, b) { return b - a })
            if (root.uiBridge.queueLength > 0 && indices.length >= root.uiBridge.queueLength) {
                root.requestViewportRestoreWindow(700)
                root.uiBridge.clearQueue()
                root.setSelectedIndices([])
                root.selectionAnchorIndex = -1
                return
            }
            root.requestViewportRestoreWindow(Math.max(700, indices.length * 120))
            for (let i = 0; i < indices.length; ++i) {
                root.uiBridge.removeAt(indices[i])
            }
            root.setSelectedIndices([])
            root.selectionAnchorIndex = -1
            return
        }
        if (root.uiBridge.selectedQueueIndex >= 0) {
            root.requestViewportRestoreWindow(700)
            root.uiBridge.removeAt(root.uiBridge.selectedQueueIndex)
        }
    }

    function handleBridgeSnapshotUpdate() {
        if (root.uiBridge.queueVersion !== root.lastSeenQueueVersion) {
            root.lastSeenQueueVersion = root.uiBridge.queueVersion
            root.resetSelectionForUpdatedQueue()
            root.applyPendingViewportRestore()
            root.syncSelectionToCurrentQueue()
            root.lastSyncedBridgeSelectedIndex = root.uiBridge.selectedQueueIndex
        }
        if (root.uiBridge.selectedQueueIndex !== root.lastSyncedBridgeSelectedIndex) {
            root.syncSelectionToCurrentQueue()
            root.lastSyncedBridgeSelectedIndex = root.uiBridge.selectedQueueIndex
        }
    }

    function initializeFromBridge() {
        root.lastSeenQueueVersion = root.uiBridge.queueVersion
        root.lastAutoCenterPlaybackState = root.uiBridge.playbackState
        root.lastAutoCenterTrackPath = root.uiBridge.currentTrackPath
        root.syncSelectionToCurrentQueue()
        root.lastSyncedBridgeSelectedIndex = root.uiBridge.selectedQueueIndex
    }
}
