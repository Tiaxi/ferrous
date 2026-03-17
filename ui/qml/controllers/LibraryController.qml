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
    property var selectedSelectionKeys: []
    property int selectionAnchorIndex: -1
    property int lastAppliedVersion: -1
    property int pendingVersion: -1
    property bool hasReceivedTreeFrame: false
    property string pendingAnchorKey: ""
    property real pendingAnchorOffset: 0
    property real pendingAnchorFallbackY: 0
    property bool pendingAnchorValid: false
    property string typeAheadBuffer: ""
    property string pendingRevealSelectionKey: ""
    property var pendingRevealExpandKeys: []
    property int pendingRevealAttempts: 0
    property string pendingExpandFitKey: ""
    property int pendingExpandFitAttempts: 0
    property string pendingSearchOpenSelectionKey: ""
    property var pendingSearchOpenExpandKeys: []
    property int pendingSearchOpenAttempts: 0
    property var view: null
    readonly property bool viewHasActiveFocus: !!root.view && root.view.activeFocus

    property Timer typeAheadTimer: Timer {
        interval: 900
        repeat: false
        onTriggered: root.typeAheadBuffer = ""
    }

    property Timer revealRetryTimer: Timer {
        interval: 80
        repeat: false
        onTriggered: root.applyPendingReveal()
    }

    property Timer searchOpenRetryTimer: Timer {
        interval: 80
        repeat: false
        onTriggered: root.applyPendingSearchOpen()
    }

    function registerView(view) {
        root.view = view
    }

    function isSelectionKeySelected(key) {
        return key.length > 0 && root.selectedSelectionKeys.indexOf(key) >= 0
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

    function appendRow(rowMap) {
        if (!root.isActionableRow(rowMap)) {
            return false
        }
        const rowType = rowMap.rowType || ""
        if (rowType === "track") {
            root.uiBridge.appendTrack(rowMap.trackPath || "")
            return true
        }
        if (rowType === "album") {
            const albumPaths = rowMap.playPaths || []
            if (albumPaths.length > 0) {
                root.uiBridge.appendPaths(albumPaths)
            } else {
                root.uiBridge.appendAlbumByKey(
                    rowMap.artist || "",
                    rowMap.selectionKey || rowMap.name || "")
            }
            return true
        }
        if (rowType === "artist") {
            root.uiBridge.appendArtistByName(rowMap.selectionKey || rowMap.artist || "")
            return true
        }
        if (rowType === "root") {
            root.uiBridge.appendRootByPath(rowMap.openPath || "")
            return true
        }
        root.uiBridge.appendPaths(rowMap.playPaths || [])
        return true
    }

    function replaceWithRow(rowMap) {
        if (!root.isActionableRow(rowMap)) {
            return false
        }
        const rowType = rowMap.rowType || ""
        if (rowType === "track") {
            root.uiBridge.playTrack(rowMap.trackPath || "")
            return true
        }
        if (rowType === "album") {
            const albumPaths = rowMap.playPaths || []
            if (albumPaths.length > 0) {
                root.uiBridge.replaceWithPaths(albumPaths)
            } else {
                root.uiBridge.replaceAlbumByKey(
                    rowMap.artist || "",
                    rowMap.selectionKey || rowMap.name || "")
            }
            return true
        }
        if (rowType === "artist") {
            root.uiBridge.replaceArtistByName(rowMap.selectionKey || rowMap.artist || "")
            return true
        }
        if (rowType === "root") {
            root.uiBridge.replaceRootByPath(rowMap.openPath || "")
            return true
        }
        root.uiBridge.replaceWithPaths(rowMap.playPaths || [])
        return true
    }

    function selectedRowsSorted() {
        const rows = []
        for (let i = 0; i < root.selectedSelectionKeys.length; ++i) {
            const key = root.selectedSelectionKeys[i] || ""
            if (key.length === 0) {
                continue
            }
            const rowIndex = root.libraryModel.indexForSelectionKey(key)
            if (rowIndex < 0) {
                continue
            }
            const rowMap = root.libraryModel.rowDataForRow(rowIndex)
            if (root.isActionableRow(rowMap)) {
                rows.push({ index: rowIndex, row: rowMap })
            }
        }
        rows.sort(function(a, b) { return a.index - b.index })
        return rows
    }

    function currentSelectionRow() {
        return {
            rowType: root.selectedRowType,
            artist: root.selectedArtist,
            name: root.selectedAlbum,
            sourceIndex: root.selectedSourceIndex,
            trackPath: root.selectedTrackPath,
            openPath: root.selectedOpenPath,
            playPaths: root.selectedPlayPaths
        }
    }

    function rowsForAction(rowMap) {
        if (rowMap
                && rowMap.selectionKey
                && root.isSelectionKeySelected(rowMap.selectionKey)
                && root.selectedSelectionKeys.length > 1) {
            const selectedRows = root.selectedRowsSorted()
            if (selectedRows.length > 0) {
                return selectedRows.map(function(entry) { return entry.row })
            }
        }
        return rowMap ? [rowMap] : []
    }

    function canPlaySelection() {
        if (root.selectedSelectionKeys.length > 0) {
            return root.selectedRowsSorted().length > 0
        }
        return root.isActionableRow(root.currentSelectionRow())
    }

    function playRows(rows) {
        if (!rows || rows.length === 0) {
            return
        }
        if (!root.replaceWithRow(rows[0])) {
            return
        }
        for (let i = 1; i < rows.length; ++i) {
            root.appendRow(rows[i])
        }
    }

    function appendRows(rows) {
        if (!rows || rows.length === 0) {
            return
        }
        for (let i = 0; i < rows.length; ++i) {
            root.appendRow(rows[i])
        }
    }

    function playSelection() {
        const rows = root.selectedRowsSorted().map(function(entry) { return entry.row })
        if (rows.length > 0) {
            root.playRows(rows)
            return
        }
        root.playRows([root.currentSelectionRow()])
    }

    function appendSelection() {
        const rows = root.selectedRowsSorted().map(function(entry) { return entry.row })
        if (rows.length > 0) {
            root.appendRows(rows)
            return
        }
        root.appendRows([root.currentSelectionRow()])
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
            root.selectedSelectionKeys = []
            root.selectionAnchorIndex = -1
            root.clearPrimarySelection()
            return
        }
        root.selectedSelectionKeys = [rowMap.selectionKey]
        root.selectionAnchorIndex = index
        root.applyPrimaryRow(rowMap)
    }

    function setRangeSelection(index) {
        const anchor = root.selectionAnchorIndex >= 0 ? root.selectionAnchorIndex : index
        const first = Math.min(anchor, index)
        const last = Math.max(anchor, index)
        const keys = []
        for (let i = first; i <= last; ++i) {
            const rowMap = root.libraryModel.rowDataForRow(i)
            const key = rowMap.selectionKey || ""
            if (key.length > 0 && keys.indexOf(key) < 0) {
                keys.push(key)
            }
        }
        root.selectedSelectionKeys = keys
        root.selectionAnchorIndex = anchor
        root.applyPrimaryFromIndex(index)
    }

    function toggleSelection(index, rowMap) {
        const key = rowMap.selectionKey || ""
        if (key.length === 0) {
            return
        }
        const keys = root.selectedSelectionKeys.slice()
        const pos = keys.indexOf(key)
        if (pos >= 0) {
            keys.splice(pos, 1)
        } else {
            keys.push(key)
        }
        root.selectedSelectionKeys = keys
        root.selectionAnchorIndex = index
        if (keys.length === 0) {
            root.clearPrimarySelection()
            return
        }
        if (keys.indexOf(root.selectedSelectionKey) >= 0) {
            return
        }
        const fallbackKey = keys[keys.length - 1]
        const fallbackIndex = root.libraryModel.indexForSelectionKey(fallbackKey)
        if (!root.applyPrimaryFromIndex(fallbackIndex)) {
            root.clearPrimarySelection()
        }
    }

    function handleRowSelection(index, rowMap, button, modifiers) {
        if (!rowMap.selectionKey || rowMap.selectionKey.length === 0) {
            return
        }
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
            root.selectionAnchorIndex = index
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
        const rowHeight = 24
        const topIndex = Math.max(0, Math.min(
            root.libraryModel.count - 1,
            Math.floor(root.view.contentY / rowHeight)))
        return {
            key: root.libraryModel.selectionKeyForRow(topIndex) || "",
            offset: root.view.contentY - (topIndex * rowHeight),
            fallbackY: root.view.contentY
        }
    }

    function restoreViewAnchor(anchor) {
        if (!root.view) {
            return
        }
        const rowHeight = 24
        let targetY = anchor && anchor.fallbackY !== undefined ? anchor.fallbackY : 0
        if (anchor && anchor.key && anchor.key.length > 0) {
            const index = root.libraryModel.indexForSelectionKey(anchor.key)
            if (index >= 0) {
                targetY = (index * rowHeight) + (anchor.offset || 0)
            }
        }
        const restoreY = function() {
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

        const rowHeight = 24
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

        const blockTop = rowIndex * rowHeight
        const blockBottom = (lastDescendantIndex + 1) * rowHeight
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

    function syncSelectionToVisibleRows() {
        const valid = []
        for (let i = 0; i < root.selectedSelectionKeys.length; ++i) {
            const key = root.selectedSelectionKeys[i]
            if (root.libraryModel.hasSelectionKey(key) && valid.indexOf(key) < 0) {
                valid.push(key)
            }
        }
        root.selectedSelectionKeys = valid
        if (root.selectedSelectionKey.length > 0
                && root.selectedSelectionKeys.indexOf(root.selectedSelectionKey) < 0) {
            if (root.selectedSelectionKeys.length > 0) {
                const fallbackIndex = root.libraryModel.indexForSelectionKey(root.selectedSelectionKeys[0])
                if (!root.applyPrimaryFromIndex(fallbackIndex)) {
                    root.clearPrimarySelection()
                }
            } else {
                root.clearPrimarySelection()
            }
        }
        if (root.selectionAnchorIndex >= root.libraryModel.count || root.selectionAnchorIndex < 0) {
            root.selectionAnchorIndex = root.selectedSelectionKey.length > 0
                ? root.libraryModel.indexForSelectionKey(root.selectedSelectionKey)
                : -1
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

    function selectRelative(delta) {
        if (root.libraryModel.count <= 0) {
            return
        }
        const current = root.currentSelectionIndex()
        const base = current >= 0 ? current : 0
        const next = Math.max(0, Math.min(root.libraryModel.count - 1, base + delta))
        root.selectIndex(next)
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
        const rowMap = root.libraryModel.rowDataForRow(index)
        const rows = root.rowsForAction(rowMap)
        if (rows.length > 0) {
            root.playRows(rows)
        }
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
        if (event.key === Qt.Key_Up) {
            root.selectRelative(-1)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Down) {
            root.selectRelative(1)
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
        if (event.key === Qt.Key_Space) {
            const index = root.currentSelectionIndex()
            if (index >= 0) {
                const rowMap = root.libraryModel.rowDataForRow(index)
                const rowType = rowMap.rowType || ""
                if (rowType !== "track" && (rowMap.key || "").length > 0 && (rowMap.count || 0) > 0) {
                    root.toggleNode(rowMap.key || "")
                }
            }
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
        root.pendingRevealExpandKeys = expandKeys
        root.pendingRevealSelectionKey = (row.trackKey || row.albumKey || row.artistKey || "")
        root.pendingRevealAttempts = 80
        Qt.callLater(root.applyPendingReveal)
    }

    function ensureKeyExpanded(key) {
        const normalized = (key || "").trim()
        if (normalized.length === 0) {
            return true
        }
        const rowIndex = root.libraryModel.indexForSelectionKey(normalized)
        if (rowIndex < 0) {
            return false
        }
        const rowMap = root.libraryModel.rowDataForRow(rowIndex)
        if (!rowMap) {
            return false
        }
        const rowType = rowMap.rowType || ""
        const hasChildren = rowType !== "track" && (rowMap.count || 0) > 0
        if (!hasChildren || !!rowMap.expanded) {
            return true
        }
        root.libraryModel.toggleKey(normalized)
        return false
    }

    function applyPendingReveal() {
        if (root.pendingRevealSelectionKey.length === 0) {
            return
        }
        for (let i = 0; i < root.pendingRevealExpandKeys.length; ++i) {
            const expandKey = root.pendingRevealExpandKeys[i] || ""
            if (expandKey.length > 0) {
                root.ensureKeyExpanded(expandKey)
            }
        }
        const index = root.libraryModel.indexForSelectionKey(root.pendingRevealSelectionKey)
        if (index >= 0) {
            root.selectIndex(index)
            root.focusViewForNavigation()
            root.pendingRevealSelectionKey = ""
            root.pendingRevealExpandKeys = []
            root.pendingRevealAttempts = 0
            return
        }
        if (root.pendingRevealAttempts <= 0) {
            root.pendingRevealSelectionKey = ""
            root.pendingRevealExpandKeys = []
            return
        }
        root.pendingRevealAttempts -= 1
        revealRetryTimer.restart()
    }

    function applyPendingSearchOpen() {
        if (root.pendingSearchOpenSelectionKey.length === 0) {
            return
        }
        for (let i = 0; i < root.pendingSearchOpenExpandKeys.length; ++i) {
            const expandKey = root.pendingSearchOpenExpandKeys[i] || ""
            if (expandKey.length > 0) {
                root.ensureKeyExpanded(expandKey)
            }
        }
        const index = root.libraryModel.indexForSelectionKey(root.pendingSearchOpenSelectionKey)
        if (index >= 0) {
            const rowMap = root.libraryModel.rowDataForRow(index)
            const openPath = rowMap.openPath || rowMap.trackPath || ""
            if (openPath.length > 0) {
                root.uiBridge.openInFileBrowser(openPath)
            }
            root.pendingSearchOpenSelectionKey = ""
            root.pendingSearchOpenExpandKeys = []
            root.pendingSearchOpenAttempts = 0
            return
        }
        if (root.pendingSearchOpenAttempts <= 0) {
            root.pendingSearchOpenSelectionKey = ""
            root.pendingSearchOpenExpandKeys = []
            return
        }
        root.pendingSearchOpenAttempts -= 1
        searchOpenRetryTimer.restart()
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
        root.pendingSearchOpenSelectionKey = selectionKey
        root.pendingSearchOpenExpandKeys = expandKeys
        root.pendingSearchOpenAttempts = 80
        Qt.callLater(root.applyPendingSearchOpen)
    }
}
