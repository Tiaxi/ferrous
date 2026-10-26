// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

QtObject {
    id: root

    required property var uiBridge
    required property var libraryModel
    required property var tryCaptureGlobalSearchPrefill
    required property var tagEditorApi
    required property var openTagEditorDialog

    property string selectedSelectionKey: ""
    property int selectedSourceIndex: -1
    property string selectedRowType: ""
    property string selectedArtist: ""
    property string selectedAlbum: ""
    property string selectedTrackPath: ""
    property string selectedOpenPath: ""
    property var selectedPlayPaths: []
    property int lastAppliedVersion: -1
    property int pendingVersion: -1
    property bool hasReceivedTreeFrame: false
    property string pendingAnchorKey: ""
    property real pendingAnchorOffset: 0
    property real pendingAnchorFallbackY: 0
    property bool pendingAnchorValid: false
    property string typeAheadBuffer: ""
    // The row the model is revealing once the backend sends it, and what
    // to do with it then: "select", "center", "open" or "restore".
    property string pendingRevealKey: ""
    property string pendingRevealPurpose: ""
    property string pendingExpandFitKey: ""
    property int pendingExpandFitAttempts: 0
    property bool restoredViewStateApplied: false
    property string pendingRestoredSelectionKey: ""
    property string pendingRestoredAnchorKey: ""
    property real pendingRestoredAnchorOffset: 0
    property var pendingRestoredExpandKeys: []
    property bool restoringViewState: false
    property bool restoreRevealStarted: false
    // The saved state was revealed before the view was registered.
    property bool restoreAwaitingView: false
    property var view: null
    readonly property bool viewHasActiveFocus: !!root.view && root.view.activeFocus

    // The height of the view's rows, which the view gets from the font.
    function rowHeight() {
        return root.view ? root.view.rowHeight : 0
    }

    property Timer typeAheadTimer: Timer {
        interval: 900
        repeat: false
        onTriggered: root.typeAheadBuffer = ""
    }

    property Timer viewStatePersistTimer: Timer {
        interval: 250
        repeat: false
        onTriggered: root.persistLibraryViewState()
    }

    // The model reveals rows the backend had not sent when they arrive.
    property Connections _modelConnections: Connections {
        target: root.libraryModel
        ignoreUnknownSignals: true

        function onRevealFinished(selectionKey, row) {
            root.handleRevealFinished(selectionKey, row)
        }
    }

    function registerView(view) {
        root.view = view
        if (root.restoreAwaitingView) {
            root.restoreAwaitingView = false
            root.finishRestoredViewState()
        }
    }

    function scheduleLibraryViewStatePersist() {
        if (root.restoredViewStateApplied && !root.restoringViewState) {
            viewStatePersistTimer.restart()
        }
    }

    function persistLibraryViewState() {
        if (!root.restoredViewStateApplied
                || root.restoringViewState
                || !root.uiBridge.setLibraryViewState) {
            return
        }
        const anchor = root.captureViewAnchor()
        root.uiBridge.setLibraryViewState(
            root.selectedSelectionKey || "",
            anchor.key || "",
            anchor.offset || 0)
    }

    function isSelectionKeySelected(key) {
        return key.length > 0 && root.libraryModel.isKeySelected(key)
    }

    function selectionCount() {
        return root.libraryModel.selectionCount || 0
    }

    function isActionableRow(rowMap) {
        if (!rowMap) {
            return false
        }
        const rowType = rowMap.rowType || ""
        if (rowType === "track") {
            return (rowMap.trackPath || "").length > 0
        }
        if (rowType === "album") {
            return (rowMap.artist || "").length > 0 && (rowMap.name || "").length > 0
                && (rowMap.playPaths || []).length > 0
        }
        if (rowType === "artist") {
            return (rowMap.artist || "").length > 0
        }
        if (rowType === "root") {
            return (rowMap.openPath || "").length > 0
        }
        const paths = rowMap.playPaths || []
        return paths.length > 0
    }

    // The rows an action on `rowMap` acts on: the selection when the row
    // is one of several selected rows, otherwise the row.
    function rowsForAction(rowMap) {
        if (rowMap
                && rowMap.selectionKey
                && root.isSelectionKeySelected(rowMap.selectionKey)
                && root.selectionCount() > 1) {
            const selectedRows = root.libraryModel.selectedRowData()
            if (selectedRows.length > 0) {
                return selectedRows
            }
        }
        return rowMap ? [rowMap] : []
    }

    function canPlaySelection() {
        return root.selectionCount() > 0 && root.libraryModel.selectionActionable
    }

    // Each queues what an action on `rowMap` acts on, as one change to the
    // queue.
    function playRowItems(rowMap) {
        root.uiBridge.playLibraryItems(root.libraryModel.queueItemsForKey(rowMap.selectionKey || ""))
    }

    function appendRowItems(rowMap) {
        root.uiBridge.appendLibraryItems(root.libraryModel.queueItemsForKey(rowMap.selectionKey || ""))
    }

    function insertRowItems(rowMap, row) {
        root.uiBridge.insertLibraryItems(
            root.libraryModel.queueItemsForKey(rowMap.selectionKey || ""), row)
    }

    function playSelection() {
        if (root.canPlaySelection()) {
            root.uiBridge.playLibraryItems(root.libraryModel.queueItemsForSelection())
        }
    }

    function appendSelection() {
        if (root.canPlaySelection()) {
            root.uiBridge.appendLibraryItems(root.libraryModel.queueItemsForSelection())
        }
    }

    function canOpenTagEditorForRow(rowMap) {
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

    function openTagEditorForRow(rowMap) {
        const rows = root.rowsForAction(rowMap)
        const selections = []
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (!root.canOpenTagEditorForRow(row)) {
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
        if (selections.length > 0 && root.tagEditorApi.openSelection(selections)) {
            root.openTagEditorDialog()
        }
    }

    function canPlayAllTracks() {
        return root.uiBridge.libraryTrackCount > 0
    }

    function playAllTracks() {
        if (root.canPlayAllTracks()) {
            root.uiBridge.replaceAllLibraryTracks()
        }
    }

    function appendAllTracks() {
        if (root.canPlayAllTracks()) {
            root.uiBridge.appendAllLibraryTracks()
        }
    }

    function clearPrimarySelection() {
        root.selectedSelectionKey = ""
        root.selectedSourceIndex = -1
        root.selectedRowType = ""
        root.selectedArtist = ""
        root.selectedAlbum = ""
        root.selectedTrackPath = ""
        root.selectedOpenPath = ""
        root.selectedPlayPaths = []
        root.scheduleLibraryViewStatePersist()
    }

    function applyPrimaryRow(rowMap) {
        root.selectedSelectionKey = rowMap.selectionKey || ""
        root.selectedSourceIndex = rowMap.sourceIndex !== undefined ? rowMap.sourceIndex : -1
        root.selectedRowType = rowMap.rowType || ""
        root.selectedArtist = rowMap.artist || ""
        root.selectedAlbum = rowMap.name || ""
        root.selectedTrackPath = rowMap.trackPath || ""
        root.selectedOpenPath = rowMap.openPath || ""
        root.selectedPlayPaths = rowMap.playPaths || []
        root.scheduleLibraryViewStatePersist()
    }

    function applyPrimaryFromIndex(index) {
        const rowMap = root.libraryModel.rowDataForRow(index)
        if (rowMap && rowMap.selectionKey && rowMap.selectionKey.length > 0) {
            root.applyPrimaryRow(rowMap)
            return true
        }
        return false
    }

    function setSingleSelection(index, rowMap) {
        if (!rowMap.selectionKey || rowMap.selectionKey.length === 0) {
            root.libraryModel.clearSelection()
            root.clearPrimarySelection()
            return
        }
        root.libraryModel.selectRow(index)
        root.applyPrimaryRow(rowMap)
    }

    function setRangeSelection(index) {
        root.libraryModel.selectRange(index)
        root.applyPrimaryFromIndex(index)
    }

    function toggleSelection(index, rowMap) {
        const key = rowMap.selectionKey || ""
        if (key.length === 0) {
            return
        }
        root.libraryModel.toggleRowSelected(index)
        if (root.selectionCount() === 0) {
            root.clearPrimarySelection()
            return
        }
        if (root.isSelectionKeySelected(root.selectedSelectionKey)) {
            return
        }
        const fallbackIndex = root.libraryModel.isRowSelected(index)
            ? index
            : root.libraryModel.firstSelectedRow()
        if (!root.applyPrimaryFromIndex(fallbackIndex)) {
            root.clearPrimarySelection()
        }
    }

    function handleRowSelection(index, rowMap, button, modifiers) {
        if (!rowMap.selectionKey || rowMap.selectionKey.length === 0) {
            return
        }
        // What the user picks wins over a row still being revealed.
        root.cancelPendingReveal()
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        if (shift) {
            root.setRangeSelection(index)
            return
        }
        if (ctrl) {
            root.toggleSelection(index, rowMap)
            return
        }
        if (button === Qt.RightButton && root.isSelectionKeySelected(rowMap.selectionKey)) {
            root.applyPrimaryRow(rowMap)
            return
        }
        root.setSingleSelection(index, rowMap)
    }

    function toggleNode(key) {
        if (!key || key.length === 0) {
            return
        }
        const index = root.libraryModel.indexForSelectionKey(key)
        let expanding = false
        if (index >= 0) {
            const rowMap = root.libraryModel.rowDataForRow(index) || ({})
            const rowType = rowMap.rowType || ""
            const hasChildren = rowType !== "track" && (rowMap.count || 0) > 0
            expanding = hasChildren && !Boolean(rowMap.expanded)
        }
        if (expanding) {
            root.scheduleExpansionFit(key)
        } else if (root.pendingExpandFitKey === key) {
            root.pendingExpandFitKey = ""
            root.pendingExpandFitAttempts = 0
        }
        root.pendingAnchorValid = false
        root.libraryModel.toggleKey(key)
        // Rows a collapse hides leave the selection.
        root.syncSelectionToVisibleRows()
        if (expanding) {
            Qt.callLater(function() {
                root.applyPendingExpansionFit()
            })
        }
    }

    function captureViewAnchor() {
        if (!root.view || root.libraryModel.count <= 0) {
            return {
                key: "",
                offset: 0,
                fallbackY: root.view ? root.view.contentY : 0
            }
        }
        let topIndex = root.view.indexAt(1, root.view.contentY + 1)
        if (topIndex < 0) {
            topIndex = Math.floor(root.view.contentY / root.rowHeight())
        }
        topIndex = Math.max(0, Math.min(root.libraryModel.count - 1, topIndex))
        const rowTop = root.rowContentY(topIndex)
        return {
            key: root.libraryModel.selectionKeyForRow(topIndex) || "",
            offset: root.view.contentY - rowTop,
            fallbackY: root.view.contentY
        }
    }

    function rowContentY(index) {
        if (!root.view || index < 0) {
            return 0
        }
        if (typeof root.view.itemAtIndex === "function") {
            const item = root.view.itemAtIndex(index)
            if (item && isFinite(item.y)) {
                return item.y
            }
        }
        const origin = isFinite(root.view.originY) ? root.view.originY : 0
        return origin + (index * root.rowHeight())
    }

    function restoreViewAnchor(anchor) {
        if (!root.view) {
            return
        }
        const restoreY = function() {
            let targetY = anchor && anchor.fallbackY !== undefined ? anchor.fallbackY : 0
            if (anchor && anchor.key && anchor.key.length > 0) {
                const index = root.libraryModel.indexForSelectionKey(anchor.key)
                if (index >= 0) {
                    root.view.positionViewAtIndex(index, ListView.Beginning)
                    targetY = root.rowContentY(index) + (anchor.offset || 0)
                }
            }
            const maxYNow = Math.max(0, root.view.contentHeight - root.view.height)
            root.view.contentY = Math.max(0, Math.min(targetY, maxYNow))
        }
        restoreY()
        Qt.callLater(restoreY)
    }

    function scheduleExpansionFit(key) {
        if (!key || key.length === 0) {
            return
        }
        root.pendingExpandFitKey = key
        root.pendingExpandFitAttempts = 4
    }

    function applyPendingExpansionFit() {
        if (!root.view || root.pendingExpandFitKey.length === 0) {
            return
        }
        const key = root.pendingExpandFitKey
        const rowIndex = root.libraryModel.indexForSelectionKey(key)
        if (rowIndex < 0 || rowIndex >= root.libraryModel.count) {
            return
        }

        const rowMap = root.libraryModel.rowDataForRow(rowIndex) || ({})
        if (!rowMap || !rowMap.expanded) {
            root.pendingExpandFitKey = ""
            root.pendingExpandFitAttempts = 0
            return
        }

        const viewHeight = Math.max(0, root.view.height)
        if (viewHeight <= 0) {
            if (root.pendingExpandFitAttempts > 0) {
                root.pendingExpandFitAttempts -= 1
                Qt.callLater(function() {
                    root.applyPendingExpansionFit()
                })
            } else {
                root.pendingExpandFitKey = ""
            }
            return
        }

        const rowHeight = root.rowHeight()
        const baseDepth = rowMap.depth !== undefined ? rowMap.depth : 0
        let lastDescendantIndex = rowIndex
        for (let i = rowIndex + 1; i < root.libraryModel.count; ++i) {
            const descendant = root.libraryModel.rowDataForRow(i) || ({})
            const descendantDepth = descendant.depth !== undefined ? descendant.depth : 0
            if (descendantDepth <= baseDepth) {
                break
            }
            lastDescendantIndex = i
        }

        if ((rowMap.count || 0) > 0 && lastDescendantIndex === rowIndex) {
            return
        }

        const blockTop = root.rowContentY(rowIndex)
        const blockBottom = root.rowContentY(lastDescendantIndex) + rowHeight
        if ((blockBottom - blockTop) > viewHeight) {
            root.view.positionViewAtIndex(rowIndex, ListView.Beginning)
        } else {
            root.view.positionViewAtIndex(lastDescendantIndex, ListView.Contain)
        }
        const visibleTop = root.view.contentY
        const visibleBottom = visibleTop + viewHeight
        const blockFits = (blockBottom - blockTop) <= viewHeight
        const blockVisible = blockFits
            ? (blockTop >= visibleTop - 0.5 && blockBottom <= visibleBottom + 0.5)
            : Math.abs(visibleTop - blockTop) <= 0.5
        if (blockVisible) {
            root.pendingExpandFitKey = ""
            root.pendingExpandFitAttempts = 0
        }
    }

    function finishPendingTreeApply() {
        if (root.pendingVersion < 0 || root.libraryModel.parsing) {
            return
        }
        root.lastAppliedVersion = root.pendingVersion
        root.pendingVersion = -1
        root.syncSelectionToVisibleRows()
        if (root.pendingAnchorValid) {
            if (root.pendingExpandFitKey.length === 0) {
                root.restoreViewAnchor({
                    key: root.pendingAnchorKey,
                    offset: root.pendingAnchorOffset,
                    fallbackY: root.pendingAnchorFallbackY
                })
            }
            root.pendingAnchorValid = false
        }
        if (root.pendingExpandFitKey.length > 0) {
            root.applyPendingExpansionFit()
        }
        root.tryApplyPersistedViewState()
    }

    function requestTreeApply(version, treeBytes) {
        if (version <= 0 && (!treeBytes || treeBytes.length === 0)) {
            return
        }
        if (version < 0 || version === root.pendingVersion) {
            return
        }
        if (version === root.lastAppliedVersion && root.pendingVersion < 0) {
            return
        }
        root.preparePersistedViewState()
        const anchor = root.captureViewAnchor()
        root.pendingAnchorKey = anchor.key || ""
        root.pendingAnchorOffset = anchor.offset || 0
        root.pendingAnchorFallbackY = anchor.fallbackY || 0
        root.pendingAnchorValid = true
        root.hasReceivedTreeFrame = true
        root.pendingVersion = version
        root.libraryModel.setLibraryTreeFromBinary(treeBytes || "")
        root.finishPendingTreeApply()
    }

    function isTreeLoading() {
        if (root.pendingVersion >= 0 || root.libraryModel.parsing) {
            return true
        }
        if (!root.hasReceivedTreeFrame && root.lastAppliedVersion <= 0) {
            return true
        }
        return root.uiBridge.libraryScanInProgress && (!root.view || root.view.count === 0)
    }

    // The model drops selected rows that left the tree; the primary row
    // moves to one that is still selected.
    function syncSelectionToVisibleRows() {
        if (root.selectedSelectionKey.length === 0
                || root.isSelectionKeySelected(root.selectedSelectionKey)) {
            return
        }
        if (!root.applyPrimaryFromIndex(root.libraryModel.firstSelectedRow())) {
            root.clearPrimarySelection()
        }
    }

    function currentSelectionIndex() {
        if (root.selectedSelectionKey.length > 0) {
            const selectedIndex = root.libraryModel.indexForSelectionKey(root.selectedSelectionKey)
            if (selectedIndex >= 0) {
                return selectedIndex
            }
        }
        if (root.libraryModel.count > 0) {
            return 0
        }
        return -1
    }

    function selectIndex(index) {
        if (index < 0 || index >= root.libraryModel.count) {
            return false
        }
        const rowMap = root.libraryModel.rowDataForRow(index)
        if (!rowMap || !(rowMap.selectionKey || "").length) {
            return false
        }
        root.setSingleSelection(index, rowMap)
        root.scrollSelectionKeyIntoView(rowMap.selectionKey || "")
        return true
    }

    function scrollSelectionKeyIntoView(selectionKey) {
        if (!root.view || !selectionKey || selectionKey.length === 0) {
            return
        }
        const immediateIndex = root.libraryModel.indexForSelectionKey(selectionKey)
        if (immediateIndex >= 0) {
            root.view.positionViewAtIndex(immediateIndex, ListView.Contain)
        }
        Qt.callLater(function() {
            if (!root.view) {
                return
            }
            const delayedIndex = root.libraryModel.indexForSelectionKey(selectionKey)
            if (delayedIndex >= 0) {
                root.view.positionViewAtIndex(delayedIndex, ListView.Contain)
            }
        })
    }

    function focusViewForNavigation() {
        if (!root.view) {
            return
        }
        root.view.forceActiveFocus()
        Qt.callLater(function() {
            if (root.view) {
                root.view.forceActiveFocus()
            }
        })
    }

    // Rows that PageUp and PageDown move by: a viewful, less one row.
    function pageStep() {
        const rowHeight = root.rowHeight()
        if (rowHeight <= 0) {
            return 1
        }
        return Math.max(1, Math.floor(root.view.height / rowHeight) - 1)
    }

    function selectRelative(delta, extendSelection) {
        if (root.libraryModel.count <= 0) {
            return
        }
        const current = root.currentSelectionIndex()
        const base = current >= 0 ? current : 0
        const next = Math.max(0, Math.min(root.libraryModel.count - 1, base + delta))
        if (extendSelection) {
            if (root.libraryModel.anchorRow() < 0) {
                root.libraryModel.selectRow(base)
            }
            root.setRangeSelection(next)
            const rowMap = root.libraryModel.rowDataForRow(next)
            root.scrollSelectionKeyIntoView(rowMap.selectionKey || "")
            return
        }
        root.selectIndex(next)
    }

    function collapseAll() {
        root.pendingAnchorValid = false
        root.pendingExpandFitKey = ""
        root.pendingExpandFitAttempts = 0
        root.libraryModel.collapseAll()
        root.syncSelectionToVisibleRows()
    }

    function expandSelection() {
        const index = root.currentSelectionIndex()
        if (index < 0) {
            return
        }
        if (root.selectedSelectionKey.length === 0) {
            root.selectIndex(index)
        }
        const rowMap = root.libraryModel.rowDataForRow(index)
        const key = rowMap.key || ""
        const rowType = rowMap.rowType || ""
        const expanded = !!rowMap.expanded
        const hasChildren = rowType !== "track" && (rowMap.count || 0) > 0 && key.length > 0
        if (hasChildren) {
            if (!expanded) {
                root.toggleNode(key)
            } else if (index + 1 < root.libraryModel.count) {
                root.selectIndex(index + 1)
            }
        }
    }

    function collapseSelection() {
        const index = root.currentSelectionIndex()
        if (index < 0) {
            return
        }
        if (root.selectedSelectionKey.length === 0) {
            root.selectIndex(index)
        }
        const rowMap = root.libraryModel.rowDataForRow(index)
        const key = rowMap.key || ""
        const expanded = !!rowMap.expanded
        const rowType = rowMap.rowType || ""
        const currentDepth = rowMap.depth !== undefined ? rowMap.depth : 0
        const hasChildren = rowType !== "track" && (rowMap.count || 0) > 0 && key.length > 0
        if (hasChildren && expanded) {
            root.toggleNode(key)
            return
        }
        for (let i = index - 1; i >= 0; --i) {
            const candidate = root.libraryModel.rowDataForRow(i)
            const candidateDepth = candidate.depth !== undefined ? candidate.depth : 0
            if (candidateDepth < currentDepth) {
                root.selectIndex(i)
                return
            }
        }
    }

    function activateSelection() {
        const index = root.currentSelectionIndex()
        if (index < 0) {
            return
        }
        if (root.selectedSelectionKey.length === 0) {
            root.selectIndex(index)
        }
        root.playRowItems(root.libraryModel.rowDataForRow(index))
    }

    function typeAheadSearch(prefix) {
        if (prefix.length === 0) {
            return false
        }
        const startRow = Math.max(0, root.currentSelectionIndex() + 1)
        const matchIndex = root.libraryModel.findArtistRowByPrefix(prefix, startRow)
        if (matchIndex >= 0) {
            root.selectIndex(matchIndex)
            return true
        }
        return false
    }

    function handleKeyPress(event) {
        if (root.tryCaptureGlobalSearchPrefill(event)) {
            return
        }
        if ((event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) !== 0) {
            return
        }
        if (root.libraryModel.count <= 0) {
            return
        }
        const extendSelection = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Up) {
            root.selectRelative(-1, extendSelection)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Down) {
            root.selectRelative(1, extendSelection)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Right) {
            root.expandSelection()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Left) {
            root.collapseSelection()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
            root.selectRelative(
                event.key === Qt.Key_PageUp ? -root.pageStep() : root.pageStep(),
                extendSelection)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
            root.selectRelative(
                event.key === Qt.Key_Home ? -root.libraryModel.count : root.libraryModel.count,
                extendSelection)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.activateSelection()
            event.accepted = true
            return
        }

        const text = event.text || ""
        if (text.length === 1 && text !== "\n" && text !== "\r" && text !== "\t") {
            const nextPrefix = (root.typeAheadBuffer + text).toLowerCase()
            root.typeAheadBuffer = nextPrefix
            typeAheadTimer.restart()
            if (root.typeAheadSearch(nextPrefix)) {
                event.accepted = true
            }
        }
    }

    function requestRevealForSearchRow(row) {
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
        root.revealRow(expandKeys, row.trackKey || row.albumKey || row.artistKey || "", "select")
    }

    function requestBridgeReveal(expandKeys, selectionKey) {
        const target = (selectionKey || "").trim()
        if (target.length === 0) {
            return
        }
        root.cancelPendingRestoredViewState()
        viewStatePersistTimer.stop()
        root.restoredViewStateApplied = true
        root.revealRow(expandKeys || [], target, "center")
    }

    // Expand `expandKeys` and act on the row of `selectionKey` as `purpose`
    // says, now if the model has the row, otherwise once the backend sends
    // it. A later reveal replaces one still waiting.
    function revealRow(expandKeys, selectionKey, purpose) {
        // The user's reveal wins over restoring the saved view, which ends
        // there, or never starts when no tree has come yet, so what the user
        // does next is saved.
        if (purpose !== "restore") {
            if (root.restoringViewState) {
                root.cancelPendingRestoredViewState()
            }
            root.restoredViewStateApplied = true
        }
        root.pendingRevealKey = ""
        root.pendingRevealPurpose = ""
        const row = root.libraryModel.revealKey(expandKeys, selectionKey)
        if (row < 0 && root.libraryModel.revealPending) {
            root.pendingRevealKey = selectionKey
            root.pendingRevealPurpose = purpose
            return
        }
        root.finishReveal(purpose, row)
    }

    function handleRevealFinished(selectionKey, row) {
        if (root.pendingRevealKey.length === 0 || selectionKey !== root.pendingRevealKey) {
            return
        }
        const purpose = root.pendingRevealPurpose
        root.pendingRevealKey = ""
        root.pendingRevealPurpose = ""
        root.finishReveal(purpose, row)
    }

    function cancelPendingReveal() {
        if (root.pendingRevealKey.length === 0) {
            return
        }
        if (root.pendingRevealPurpose === "restore") {
            root.cancelPendingRestoredViewState()
            root.scheduleLibraryViewStatePersist()
        }
        root.pendingRevealKey = ""
        root.pendingRevealPurpose = ""
        root.libraryModel.cancelReveal()
    }

    function finishReveal(purpose, row) {
        if (purpose === "restore") {
            root.finishRestoredViewState()
            return
        }
        if (row < 0) {
            return
        }
        const rowMap = root.libraryModel.rowDataForRow(row)
        if (purpose === "open") {
            const openPath = rowMap.openPath || rowMap.trackPath || ""
            if (openPath.length > 0) {
                root.uiBridge.openInFileBrowser(openPath)
            }
            return
        }
        if (purpose === "center") {
            root.setSingleSelection(row, rowMap)
            root.centerRow(row)
        } else {
            root.selectIndex(row)
        }
        root.focusViewForNavigation()
    }

    // Lay the view out with the rows it has now, then center `row`.
    function centerRow(row) {
        if (!root.view) {
            return
        }
        if (typeof root.view.forceLayout === "function") {
            root.view.forceLayout()
        }
        root.view.positionViewAtIndex(row, ListView.Center)
        root.scheduleLibraryViewStatePersist()
    }

    function cancelPendingRestoredViewState() {
        if (root.pendingRevealPurpose === "restore") {
            root.pendingRevealKey = ""
            root.pendingRevealPurpose = ""
            root.libraryModel.cancelReveal()
        }
        root.pendingRestoredSelectionKey = ""
        root.pendingRestoredAnchorKey = ""
        root.pendingRestoredExpandKeys = []
        root.restoringViewState = false
        root.restoreAwaitingView = false
    }

    function showTrackInLibrary(path) {
        const normalized = (path || "").trim()
        if (normalized.length > 0 && root.uiBridge.showTrackInLibrary) {
            root.uiBridge.showTrackInLibrary(normalized)
        }
    }

    function preparePersistedViewState() {
        if (root.restoredViewStateApplied
                || root.uiBridge.libraryTrackCount <= 0
                || root.uiBridge.libraryViewStateAvailable === false) {
            return false
        }
        root.restoredViewStateApplied = true
        root.restoringViewState = true
        root.restoreRevealStarted = false
        root.pendingRestoredSelectionKey = root.uiBridge.libraryViewSelectionKey || ""
        root.pendingRestoredAnchorKey = root.uiBridge.libraryViewAnchorKey || ""
        root.pendingRestoredAnchorOffset = root.uiBridge.libraryViewAnchorOffset || 0
        root.pendingRestoredExpandKeys = root.uiBridge.libraryExpandedKeys || []
        if (root.libraryModel.setExpandedKeys) {
            root.libraryModel.setExpandedKeys(root.pendingRestoredExpandKeys)
        }
        return true
    }

    // Once the first tree is in the model, reveal the saved selection, or
    // the saved top row, with the saved expansions.
    function tryApplyPersistedViewState() {
        if (!root.hasReceivedTreeFrame) {
            return
        }
        root.preparePersistedViewState()
        if (!root.restoringViewState || root.restoreRevealStarted || root.libraryModel.parsing) {
            return
        }
        root.restoreRevealStarted = true
        const target = root.pendingRestoredSelectionKey.length > 0
            ? root.pendingRestoredSelectionKey
            : root.pendingRestoredAnchorKey
        if (target.length === 0) {
            root.finishRestoredViewState()
            return
        }
        root.revealRow(root.pendingRestoredExpandKeys, target, "restore")
    }

    function finishRestoredViewState() {
        if (!root.restoringViewState) {
            return
        }
        // The top row goes back once the view can show it.
        if (!root.view) {
            root.restoreAwaitingView = true
            return
        }
        if (root.pendingRestoredSelectionKey.length > 0) {
            const selectionIndex = root.libraryModel.indexForSelectionKey(
                root.pendingRestoredSelectionKey)
            if (selectionIndex >= 0) {
                root.setSingleSelection(
                    selectionIndex, root.libraryModel.rowDataForRow(selectionIndex))
            }
        }
        if (root.pendingRestoredAnchorKey.length > 0) {
            const anchorIndex = root.libraryModel.indexForSelectionKey(
                root.pendingRestoredAnchorKey)
            if (anchorIndex >= 0) {
                if (typeof root.view.forceLayout === "function") {
                    root.view.forceLayout()
                }
                root.restoreViewAnchor({
                    key: root.pendingRestoredAnchorKey,
                    offset: root.pendingRestoredAnchorOffset,
                    fallbackY: anchorIndex * root.rowHeight()
                })
            }
        }
        root.pendingRestoredSelectionKey = ""
        root.pendingRestoredAnchorKey = ""
        root.pendingRestoredExpandKeys = []
        root.restoringViewState = false
        root.scheduleLibraryViewStatePersist()
    }

    function requestOpenInFileBrowserForSearchRow(row) {
        if (!row || row.kind !== "item") {
            return
        }
        const rowType = row.rowType || ""
        if (rowType === "track") {
            root.uiBridge.openContainingFolder(row.trackPath || "")
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
        root.revealRow(expandKeys, selectionKey, "open")
    }
}
