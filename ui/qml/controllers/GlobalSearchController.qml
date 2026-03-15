import QtQuick 2.15

QtObject {
    id: root

    required property var uiBridge
    required property var globalSearchModelApi
    required property var requestLibraryRevealForSearchRow
    required property var focusLibraryViewForNavigation
    required property var requestOpenInFileBrowserForSearchRow

    property int selectedDisplayIndex: -1
    property bool opening: false
    property bool ignoreRefocusFind: false
    property var dialog: null
    property var queryField: null
    property var resultsView: null
    property string pendingPrefillText: ""
    property string openInitialText: ""
    readonly property bool dialogVisible: !!root.dialog && root.dialog.visible
    readonly property bool dialogHasActiveInputFocus: root.dialogVisible
        && ((root.queryField && root.queryField.activeFocus)
            || (root.resultsView && root.resultsView.activeFocus))

    property Timer openSettleTimer: Timer {
        interval: 260
        repeat: false
        onTriggered: root.ignoreRefocusFind = false
    }

    function registerRefs(dialog, queryField, resultsView) {
        root.dialog = dialog
        root.queryField = queryField
        root.resultsView = resultsView
    }

    function rowCount() {
        return root.resultsView ? (root.resultsView.count || 0) : 0
    }

    function syncSelectionAfterResultsChange() {
        const firstIndex = root.nextSelectableIndex(-1, 1, false)
        if (root.selectedDisplayIndex < 0 || !root.isSearchRowSelectable(root.selectedDisplayIndex)) {
            root.selectedDisplayIndex = firstIndex
        } else if (root.selectedDisplayIndex >= root.rowCount()) {
            root.selectedDisplayIndex = firstIndex
        }
    }

    function searchFirstSelectableIndex() {
        return root.nextSelectableIndex(-1, 1, false)
    }

    function searchLastSelectableIndex() {
        return root.nextSelectableIndex(root.rowCount(), -1, false)
    }

    function isSearchRowSelectable(index) {
        return root.globalSearchModelApi ? !!root.globalSearchModelApi.isSelectableIndex(index) : false
    }

    function nextSelectableIndex(startIndex, step, wrap) {
        if (!root.globalSearchModelApi) {
            return -1
        }
        return root.globalSearchModelApi.nextSelectableIndex(startIndex, step, wrap)
    }

    function moveSelectionByPage(direction) {
        if (root.rowCount() === 0) {
            return false
        }
        const stepDir = direction < 0 ? -1 : 1
        const pageRows = Math.max(
            1,
            Math.floor(((root.resultsView ? root.resultsView.height : 240) / 24)) - 1)
        let index = root.selectedDisplayIndex
        if (!root.isSearchRowSelectable(index)) {
            index = stepDir > 0 ? root.searchFirstSelectableIndex() : root.searchLastSelectableIndex()
        }
        if (index < 0) {
            return false
        }
        let moved = false
        for (let i = 0; i < pageRows; ++i) {
            const next = root.nextSelectableIndex(index, stepDir, false)
            if (next < 0) {
                break
            }
            index = next
            moved = true
        }
        if (!moved) {
            return false
        }
        return root.selectDisplayIndex(index)
    }

    function selectDisplayIndex(index) {
        if (!root.isSearchRowSelectable(index)) {
            return false
        }
        root.selectedDisplayIndex = index
        if (root.resultsView && index >= 0 && index < root.rowCount()) {
            const firstSelectable = root.searchFirstSelectableIndex()
            if (index === firstSelectable && root.globalSearchModelApi) {
                root.resultsView.contentY = 0
                Qt.callLater(function() {
                    if (root.resultsView) {
                        root.resultsView.contentY = 0
                    }
                })
            } else {
                root.resultsView.positionViewAtIndex(index, ListView.Contain)
            }
        }
        return true
    }

    function selectedRow() {
        if (!root.isSearchRowSelectable(root.selectedDisplayIndex)) {
            return null
        }
        const row = root.globalSearchModelApi
            ? root.globalSearchModelApi.rowDataAt(root.selectedDisplayIndex)
            : null
        return row || null
    }

    function openDialog() {
        if (root.dialogVisible) {
            root.focusQueryField(!root.ignoreRefocusFind)
            return
        }
        root.beginOpen()
        if (root.dialog) {
            root.dialog.open()
        }
    }

    function focusQueryField(selectAll) {
        if (!root.queryField) {
            return
        }
        root.queryField.forceActiveFocus()
        if (selectAll) {
            root.queryField.selectAll()
        } else {
            root.queryField.cursorPosition = (root.queryField.text || "").length
        }
    }

    function beginOpen() {
        root.opening = true
        root.ignoreRefocusFind = true
        root.pendingPrefillText = ""
        root.openInitialText = root.queryField ? (root.queryField.text || "") : ""
    }

    function handleDialogOpened(queryText) {
        root.opening = false
        root.ignoreRefocusFind = true
        openSettleTimer.restart()
        root.syncSelectionAfterResultsChange()
        root.focusQueryField(false)
        root.applyOpenText()
        root.uiBridge.setGlobalSearchQuery(queryText || "")
    }

    function endOpen(closeDialog) {
        root.opening = false
        root.ignoreRefocusFind = false
        openSettleTimer.stop()
        root.pendingPrefillText = ""
        root.openInitialText = ""
        if (closeDialog) {
            root.uiBridge.setGlobalSearchQuery("")
        }
    }

    function isPrintableChar(text) {
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

    function applyOpenText() {
        if (!root.queryField) {
            return
        }
        if ((root.pendingPrefillText || "").length > 0) {
            root.queryField.text = root.pendingPrefillText
            root.pendingPrefillText = ""
            return
        }

        const current = root.queryField.text || ""
        const initial = root.openInitialText || ""
        if (current.length <= 0) {
            return
        }
        const trimmed = root.trimInitialSearchPrefix(current, initial)
        if (trimmed !== current) {
            root.queryField.text = trimmed
            root.queryField.cursorPosition = (root.queryField.text || "").length
            return
        }
        if (current === initial) {
            root.queryField.selectAll()
        }
    }

    function tryCapturePrefill(event) {
        const shouldCapture = root.opening
            || (root.dialogVisible
                && root.ignoreRefocusFind
                && (!root.queryField || !root.queryField.activeFocus))
        if (!shouldCapture) {
            return false
        }
        if ((event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) !== 0) {
            return false
        }
        const openingText = event.text || ""
        if (!root.isPrintableChar(openingText)) {
            return false
        }
        if (root.dialogVisible && !root.opening && root.queryField) {
            const hasSelection = (root.queryField.selectedText || "").length > 0
            const current = root.queryField.text || ""
            if (hasSelection) {
                root.queryField.text = openingText
            } else {
                const alreadyTyped = root.trimInitialSearchPrefix(current, root.openInitialText || "")
                root.queryField.text = alreadyTyped + openingText
            }
            root.queryField.cursorPosition = (root.queryField.text || "").length
            root.focusQueryField(false)
        } else {
            root.pendingPrefillText += openingText
        }
        event.accepted = true
        return true
    }

    function navigateSelectionToLibrary() {
        let row = root.selectedRow()
        if (!row) {
            const first = root.searchFirstSelectableIndex()
            if (first >= 0) {
                root.selectDisplayIndex(first)
                row = root.selectedRow()
            }
        }
        if (!row) {
            return
        }
        root.requestLibraryRevealForSearchRow(row)
        if (root.dialog) {
            root.dialog.close()
        }
        Qt.callLater(root.focusLibraryViewForNavigation)
    }

    function activateRow(row) {
        if (!row || row.kind !== "item") {
            return
        }
        const rowType = row.rowType || ""
        if (rowType === "track") {
            root.uiBridge.replaceWithPaths([row.trackPath || ""])
        } else if (rowType === "album") {
            const albumName = (row.album || row.label || "").trim()
            root.uiBridge.replaceAlbumByKey(
                (row.artistKey || row.artist || "").trim(),
                (row.albumKey || albumName).trim())
        } else if (rowType === "artist") {
            root.uiBridge.replaceArtistByName((row.artistKey || row.artist || row.label || "").trim())
        }
        root.requestLibraryRevealForSearchRow(row)
        if (root.dialog) {
            root.dialog.close()
        }
    }

    function queueRow(row) {
        if (!row || row.kind !== "item") {
            return
        }
        const rowType = row.rowType || ""
        if (rowType === "track") {
            root.uiBridge.appendTrack(row.trackPath || "")
            return
        }
        if (rowType === "album") {
            const albumName = (row.album || row.label || "").trim()
            root.uiBridge.appendAlbumByKey(
                (row.artistKey || row.artist || "").trim(),
                (row.albumKey || albumName).trim())
            return
        }
        if (rowType === "artist") {
            root.uiBridge.appendArtistByName((row.artistKey || row.artist || row.label || "").trim())
        }
    }

    function openRowInFileBrowser(row) {
        root.requestOpenInFileBrowserForSearchRow(row)
    }

    function activateSelection() {
        const row = root.selectedRow()
        if (row) {
            root.activateRow(row)
        }
    }

    function rowHeight(index) {
        if (index < 0 || !root.globalSearchModelApi) {
            return 24
        }
        const row = root.globalSearchModelApi.rowDataAt(index)
        return row && (row.kind || "") === "section" ? 30 : 24
    }

    function rowTop(index) {
        if (index <= 0) {
            return 0
        }
        let y = 0
        for (let i = 0; i < index; ++i) {
            y += root.rowHeight(i)
        }
        return y
    }

    function stepResultsView(wheel) {
        if (!root.resultsView || !wheel) {
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
        const maxY = Math.max(0, root.resultsView.contentHeight - root.resultsView.height)
        if (maxY <= 0) {
            return
        }
        const rowPx = 24
        const stepPx = rowPx * 3
        const notches = (wheel.angleDelta && wheel.angleDelta.y !== undefined && wheel.angleDelta.y !== 0)
            ? Math.max(1, Math.round(Math.abs(wheel.angleDelta.y) / 120))
            : Math.max(1, Math.round(Math.abs(deltaY) / stepPx))
        const direction = deltaY > 0 ? -1 : 1
        let targetY = root.resultsView.contentY + (direction * notches * stepPx)
        targetY = Math.max(0, Math.min(maxY, targetY))

        if (direction > 0 && root.rowCount() > 0) {
            const lastIndex = root.rowCount() - 1
            const lastRowTop = root.rowTop(lastIndex)
            const lastRowBottom = lastRowTop + root.rowHeight(lastIndex)
            const viewportBottom = targetY + root.resultsView.height
            const lastRowPartiallyVisible = lastRowTop < viewportBottom && lastRowBottom > viewportBottom
            if (lastRowPartiallyVisible) {
                targetY = Math.max(0, Math.min(maxY, lastRowBottom - root.resultsView.height))
            } else if ((maxY - targetY) <= root.rowHeight(lastIndex)) {
                targetY = maxY
            }
        }

        root.resultsView.contentY = targetY
        if (direction > 0 && root.rowCount() > 0) {
            const lastIndex = root.rowCount() - 1
            const probeX = Math.max(0, Math.min(8, root.resultsView.width - 1))
            const probeY = Math.max(0, root.resultsView.height - 2)
            const bottomIndex = root.resultsView.indexAt(probeX, probeY)
            if (bottomIndex >= lastIndex - 1
                    || (maxY - root.resultsView.contentY) <= root.rowHeight(lastIndex)) {
                root.resultsView.positionViewAtIndex(lastIndex, ListView.End)
                Qt.callLater(function() {
                    if (root.resultsView) {
                        root.resultsView.positionViewAtIndex(lastIndex, ListView.End)
                    }
                })
            }
        }
        wheel.accepted = true
    }
}
