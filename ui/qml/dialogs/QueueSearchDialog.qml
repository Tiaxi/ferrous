// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/ColorUtils.js" as ColorUtils

// Find in Queue (#814): the search palette's frame, field, rows and keys,
// for the queue alone. The results are queue rows in queue order, each
// with its place in the queue; Enter plays the row without replacing the
// queue, Ctrl+Enter marks it to play next, and Alt+Enter shows it in the
// queue. The queue's selection and scroll stay as they are until one of
// them acts.
Dialog {
    id: root
    objectName: "queueSearchDialog"
    required property var uiBridge
    required property var uiPalette
    required property var windowRoot
    // Selects queue row `row` in the queue and shows it there, with the
    // keyboard on the queue.
    required property var showQueueRow
    // Gives the queue the keyboard, once the palette has closed and given
    // back the focus it took.
    property var focusQueue: function() {}
    // Whether the queue takes the keyboard as the palette closes.
    property bool focusQueueOnClose: false
    // The transport: the palette opens 32 px under it, as global search's.
    property Item anchorItem: null
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    property int fadeDuration: Kirigami.Units.shortDuration

    readonly property var resultsModel: root.uiBridge.queueSearchModel || null
    readonly property bool searching: !!root.uiBridge.queueSearchBusy
    readonly property string queryText: queryField.text.trim()
    // The query the rows were found by, whose matches they show.
    property string resultsQuery: ""
    readonly property int rowCount: root.resultsModel ? (root.resultsModel.count || 0) : 0
    readonly property bool resultsShown: root.queryText.length > 0 && root.rowCount > 0
    readonly property bool noResults: root.queryText.length > 0 && !root.searching && root.rowCount === 0
    // The selected result; -1 for none.
    property int selectedIndex: -1
    // What Enter, Ctrl+Enter or Alt+Enter pressed while the rows were not
    // yet the results of the query as typed does once they are:
    // "play", "playNext" or "show", as in global search (#723).
    property string pendingAction: ""
    // Counts the rows' changes, for what reads the selected row's data.
    property int rowsRevision: 0

    // The palette's sizes, as global search's.
    readonly property color clearGlyphRest: root.uiPalette.readableOn(
        ColorUtils.mixColor(root.uiPalette.textSecondary, root.uiPalette.view, 0.45), [root.uiPalette.view], 3.1)
    readonly property color nowOnScrim: "#f2b035"
    readonly property real fieldHeight: Math.max(44, Math.round(regularMetrics.height) + 20)
    readonly property real resultHeight: Math.max(32, Math.round(regularMetrics.height) + 12)
    readonly property real footerLineHeight: Math.round(smallMetrics.height) + 4
    readonly property real footerHeight: Math.max(28, footerHints.implicitHeight + 8)
    readonly property real hintHeight: Math.max(20, Math.round(smallMetrics.height) + 4)
    readonly property real messageGap: root.uiPalette.scaledSize(8)
    readonly property real hintKeyGap: Math.max(Kirigami.Units.mediumSpacing,
                                                Math.round(1.5 * smallMetrics.advanceWidth(" ")))
    readonly property real hintPairGap: 3 * root.hintKeyGap
    readonly property real sideInset: 16
    readonly property int glyphSize: 2 * Math.round(root.uiPalette.toolIconSize / 2)
    readonly property int nowGlyphSize: 2 * Math.round(root.uiPalette.scaledSize(12) / 2)
    readonly property int leadSize: 2 * Math.floor(Math.min(root.uiPalette.scaledSize(24), root.resultHeight - 8) / 2)
    readonly property int fieldButtonSize: 2 * Math.floor(Math.min(root.uiPalette.scaledSize(28), root.fieldHeight - 16) / 2)
    readonly property real columnGap: 12
    // The place in the queue before the lead, right-aligned in a column as
    // wide as the queue's largest place, in tabular figures, as the
    // queue's own column; the lead and the title after it.
    readonly property real placeWidth: Math.ceil(placeMetrics.advanceWidth)
    readonly property real leadX: root.sideInset + root.placeWidth + Kirigami.Units.largeSpacing
    readonly property real titleX: root.leadX + root.leadSize + root.columnGap
    readonly property real lengthWidth: Math.max(44, Math.ceil(lengthMetrics.advanceWidth))
    readonly property real titleWidth: {
        const lengthLeft = root.width - root.sideInset - Kirigami.Units.largeSpacing - root.lengthWidth - root.columnGap
        const room = lengthLeft - root.columnGap - root.titleX
        const least = Math.min(root.uiPalette.scaledSize(300), Math.round(room * 0.6))
        const spared = room - root.uiPalette.scaledSize(276)
        return Math.max(0, Math.min(root.uiPalette.scaledSize(380), Math.max(least, spared)))
    }
    readonly property real contextX: root.titleX + root.titleWidth + root.columnGap
    property real paletteTop: 32
    readonly property int leastResultRows: 5
    function updatePlacement() {
        const windowItem = root.parent
        if (!windowItem) {
            root.paletteTop = 32
            return
        }
        const underTransport = root.anchorItem
            ? Math.round(root.anchorItem.mapToItem(windowItem, 0, root.anchorItem.height).y) + 1 + 32
            : 32
        const leastHeight = root.fieldHeight + 1 + root.leastResultRows * root.resultHeight + 1
            + root.footerHeightFor(root.hintsFor(true))
        const menuBar = root.windowRoot && root.windowRoot.menuBar && root.windowRoot.menuBar.visible
            ? root.windowRoot.menuBar : null
        const highest = (menuBar ? Math.ceil(menuBar.mapToItem(windowItem, 0, menuBar.height).y) : 0) + 8
        root.paletteTop = Math.max(highest, Math.min(underTransport, Math.floor(windowItem.height - 32 - leastHeight)))
    }
    readonly property bool selectionActive: !resultMenu.visible

    readonly property bool closing: exitFade.running
    modal: !root.closing
    closePolicy: root.closing ? Popup.NoAutoClose : Popup.CloseOnEscape
    dim: false
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: undefined
    margins: 0
    title: I18n.i18nc("@title:window", "Find in Queue")
    padding: 0
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0
    readonly property int designWidth: root.uiPalette.scaledSize(800)
    width: Math.min(root.designWidth, (root.parent ? root.parent.width : root.designWidth) - 2 * root.sideInset)
    height: Math.min(contentColumn.implicitHeight, root.maximumHeight)
    readonly property real maximumBodyHeight: 16 * root.resultHeight
    readonly property real maximumHeight: Math.max(root.fieldHeight + root.footerHeight + 2,
        Math.min(root.fieldHeight + 1 + root.maximumBodyHeight + 1 + root.footerHeight,
                 (root.parent ? root.parent.height : 600) - root.paletteTop - 32))
    x: Math.round(((root.parent ? root.parent.width : root.width) - root.width) / 2)
    y: Math.round(root.paletteTop)
    header: null
    footer: null
    onAboutToShow: root.updatePlacement()

    background: Kirigami.ShadowedRectangle {
        objectName: "queueSearchBackground"
        color: root.uiPalette.view
        radius: 6
        border.width: 1
        border.color: root.uiPalette.seam
        shadow.xOffset: 0
        shadow.yOffset: 4
        shadow.size: 8
        shadow.color: Qt.rgba(0, 0, 0, 0.3)

        // The dim over the whole window, as global search's: the window
        // takes no hover or presses while the palette is open, and a press
        // outside closes it.
        Rectangle {
            objectName: "queueSearchDim"
            z: -1
            x: -root.x
            y: -root.y
            width: root.parent ? root.parent.width : 0
            height: root.parent ? root.parent.height : 0
            color: root.uiPalette.dim

            MouseArea {
                anchors.fill: parent
                enabled: !root.closing
                acceptedButtons: Qt.AllButtons
                hoverEnabled: true
                cursorShape: Qt.ArrowCursor
                onPressed: function(mouse) {
                    const point = mapToItem(root.background, mouse.x, mouse.y)
                    mouse.accepted = !root.background.contains(point)
                    if (mouse.accepted) {
                        root.reject()
                    }
                }
            }
        }
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"; from: 0; to: 1
            duration: root.fadeDuration; easing.type: Easing.OutCubic
        }
    }
    exit: Transition {
        id: exitFade
        NumberAnimation {
            property: "opacity"; from: 1; to: 0
            duration: root.fadeDuration; easing.type: Easing.OutCubic
        }
    }

    // Opens the palette with the query field focused, or, while it is open,
    // focuses the field again with its query selected.
    function openPalette() {
        if (root.visible && !root.closing) {
            root.focusQueryField(true)
            return
        }
        root.pendingAction = ""
        root.focusQueueOnClose = false
        root.open()
    }
    function focusQueryField(selectAll) {
        queryField.forceActiveFocus()
        if (selectAll) queryField.selectAll()
        else queryField.cursorPosition = queryField.text.length
    }
    onOpened: {
        // The query kept from the last time is searched again, selected,
        // so typing replaces it.
        root.uiBridge.setQueueSearchQuery(queryField.text)
        root.focusQueryField(true)
    }
    onAboutToHide: root.pendingAction = ""
    // Closed, the palette searches nothing: the queue's edits start no
    // searches, and the backend lets its copy of the queue go.
    onClosed: {
        root.uiBridge.setQueueSearchQuery("")
        if (root.focusQueueOnClose) {
            root.focusQueueOnClose = false
            root.focusQueue()
        }
    }

    // The keys act on the results of the query as typed, and wait for them
    // while a search for it runs.
    readonly property bool rowsMatchQuery: !root.searching
    onRowsMatchQueryChanged: if (root.rowsMatchQuery) root.takePendingAction()
    function deferUntilRowsMatchQuery(action) {
        root.pendingAction = root.rowsMatchQuery ? "" : action
        return root.pendingAction !== ""
    }
    function takePendingAction() {
        const action = root.pendingAction
        root.pendingAction = ""
        if (!root.visible) return
        if (action === "play") root.playSelection()
        else if (action === "playNext") root.togglePlayNextOfSelection()
        else if (action === "show") root.showSelectionInQueue()
    }

    // The queue row of the selected result, or -1.
    function selectedQueueRow() {
        return root.resultsModel && root.selectedIndex >= 0 ? root.resultsModel.queueRowAt(root.selectedIndex) : -1
    }
    function selectIndex(index) {
        if (!root.resultsModel || index < 0 || index >= root.resultsModel.rowCount()) return false
        root.selectedIndex = index
        resultsView.positionViewAtIndex(index, ListView.Contain)
        return true
    }
    function moveSelection(step, wrap) {
        if (root.rowCount === 0) return
        let next = root.selectedIndex < 0 ? (step > 0 ? 0 : root.rowCount - 1) : root.selectedIndex + step
        if (wrap) next = (next + root.rowCount) % root.rowCount
        root.selectIndex(Math.max(0, Math.min(root.rowCount - 1, next)))
    }
    readonly property int pageRows: Math.max(1, Math.floor(resultsView.height / root.resultHeight) - 1)

    // Enter: play the row, without replacing the queue, and close.
    function playSelection() {
        if (root.deferUntilRowsMatchQuery("play")) return
        const row = root.selectedQueueRow()
        if (row < 0) return
        root.uiBridge.jumpToQueueRow(row)
        root.close()
    }
    // Ctrl+Enter: mark the row to play next, or unmark it; the palette
    // stays open for more, as global search's Add to Queue.
    function togglePlayNextOfSelection() {
        if (root.deferUntilRowsMatchQuery("playNext")) return
        const row = root.selectedQueueRow()
        if (row >= 0) root.uiBridge.togglePlayNext([row])
    }
    // Alt+Enter: select the row in the queue and show it there.
    function showSelectionInQueue() {
        if (root.deferUntilRowsMatchQuery("show")) return
        const row = root.selectedQueueRow()
        if (row < 0) return
        root.focusQueueOnClose = true
        root.close()
        root.showQueueRow(row)
    }

    // The keys of the field and the results.
    function handleResultKeys(event) {
        const enterKey = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
        if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_F) {
            root.focusQueryField(true)
        } else if (enterKey) {
            if (event.modifiers & Qt.AltModifier) root.showSelectionInQueue()
            else if (event.modifiers & Qt.ControlModifier) root.togglePlayNextOfSelection()
            else root.playSelection()
        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
            root.pendingAction = ""
            root.moveSelection(event.key === Qt.Key_Down ? 1 : -1, true)
        } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
            root.pendingAction = ""
            root.moveSelection(event.key === Qt.Key_PageDown ? root.pageRows : -root.pageRows, false)
        } else return false
        event.accepted = true
        return true
    }

    Connections {
        target: root.resultsModel
        ignoreUnknownSignals: true
        // New results: the first row for a new query; for the same query,
        // searched again after the queue changed, the row selected before
        // where it still is.
        function onModelAboutToBeReset() {
            root.keptQueueRow = root.resultsQuery === queryField.text ? root.selectedQueueRow() : -1
        }
        function onModelReset() {
            root.resultsQuery = queryField.text
            root.rowsRevision += 1
            resultsView.cancelFlick()
            const kept = root.keptQueueRow >= 0 && root.resultsModel ? root.resultsModel.indexOfQueueRow(root.keptQueueRow) : -1
            root.keptQueueRow = -1
            if (kept >= 0) {
                root.selectIndex(kept)
            } else {
                // The model's own count: the palette's follows its signal,
                // which comes after the reset.
                root.selectedIndex = root.resultsModel && root.resultsModel.rowCount() > 0 ? 0 : -1
                resultsView.positionViewAtBeginning()
            }
        }
        // A row the queue lost leaves the results; the selection stays on
        // its row, or moves to the row after it.
        function onRowsRemoved(parent, first, last) {
            root.rowsRevision += 1
            if (root.selectedIndex > last) {
                root.selectedIndex -= last - first + 1
            } else if (root.selectedIndex >= first) {
                root.selectedIndex = Math.min(first, root.resultsModel.rowCount() - 1)
            }
        }
        function onLayoutAboutToBeChanged() {
            root.keptQueueRow = root.selectedQueueRow()
        }
        function onLayoutChanged() {
            root.rowsRevision += 1
            root.selectedIndex = root.keptQueueRow >= 0 ? root.resultsModel.indexOfQueueRow(root.keptQueueRow) : -1
            root.keptQueueRow = -1
        }
        function onDataChanged() {
            root.rowsRevision += 1
        }
    }
    property int keptQueueRow: -1

    // The queue's place of a row, as the queue's column writes it.
    function placeText(queueRow) {
        return Number(queueRow + 1).toLocaleString(Qt.locale(), "f", 0)
    }
    function querySegments(value) {
        const text = String(value || "")
        return root.resultsModel && root.resultsModel.highlightSegments
            ? root.resultsModel.highlightSegments(text, root.resultsQuery)
            : [{ text: text, match: false }]
    }
    function queryMatches(value) {
        return !!value && root.querySegments(value).some(function(segment) { return segment.match })
    }
    readonly property string contextSeparator: " · "
    // A result's details after its title: who and where it is from, and
    // its genre where the query matched it.
    function rowContext(row) {
        const parts = [[row.artist || "", row.album || ""].filter(function(value) { return value.length > 0 }).join(" — ")]
        if (row.genre && root.queryMatches(row.genre)) parts.push(row.genre)
        return parts.filter(function(value) { return value.length > 0 }).join(root.contextSeparator)
    }
    // The title in up to five runs, the matches semibold, as global search
    // lays its titles out.
    readonly property int maxTitleRuns: 5
    function titleRuns(label, allEmphasized) {
        const text = String(label || "")
        let segments = allEmphasized ? [{ text: text, match: true }] : root.querySegments(text)
        if (segments.length > root.maxTitleRuns) {
            const tail = segments.slice(root.maxTitleRuns - 1).map(function(segment) { return segment.text }).join("")
            segments = segments.slice(0, root.maxTitleRuns - 1).concat([{ text: tail, match: false }])
        }
        let runs = []
        let x = 0
        for (let i = 0; i < segments.length; ++i) {
            const metrics = segments[i].match ? semiboldMetrics : regularMetrics
            const advance = metrics.advanceWidth(segments[i].text)
            runs.push({ text: segments[i].text, match: !!segments[i].match, x: Math.round(x),
                        width: Math.min(advance, Math.max(0, root.titleWidth - x)), cut: x + advance > root.titleWidth + 0.5 })
            x += advance
        }
        return runs
    }
    FontMetrics { id: regularMetrics; font.family: root.uiPalette.baseFont.family; font.pointSize: root.uiPalette.basePointSize }
    FontMetrics { id: semiboldMetrics; font.family: root.uiPalette.baseFont.family; font.pointSize: root.uiPalette.basePointSize; font.weight: Font.DemiBold }
    FontMetrics { id: smallMetrics; font.family: root.uiPalette.baseFont.family; font.pointSize: root.uiPalette.smallPointSize }
    TextMetrics {
        id: lengthMetrics
        text: "00:00"
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
    }
    // As wide as the queue's last place, at least three figures.
    TextMetrics {
        id: placeMetrics
        text: root.placeText(Math.max(99, (root.uiBridge.queueLength || 0) - 1))
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
    }

    // The selected row's data, for the key hints and the menu.
    readonly property var selectedRow: root.rowsRevision >= 0 && root.selectedIndex >= 0 && root.resultsModel
        && root.selectedIndex < root.rowCount ? root.resultsModel.rowDataAt(root.selectedIndex) : null
    // Whether Ctrl+Enter unmarks the selected row rather than marking it.
    readonly property bool selectedPlaysNext: !!root.selectedRow && (root.selectedRow.playNext || 0) > 0
    readonly property var keyHints: root.hintsFor(root.resultsShown && !!root.selectedRow)
    function hintsFor(withResult) {
        const close = { key: I18n.i18nc("@info:shortcut key name", "Esc"), action: I18n.i18nc("@info:shortcut action", "Close") }
        if (!withResult) return [close]
        const pairs = [
            { key: I18n.i18nc("@info:shortcut key name", "Enter"), action: I18n.i18nc("@info:shortcut action", "Play") },
            { key: I18n.i18nc("@info:shortcut key name", "Ctrl+Enter"),
              action: root.selectedPlaysNext ? I18n.i18nc("@info:shortcut action", "Don’t Play Next")
                                             : I18n.i18nc("@info:shortcut action", "Play Next") },
            { key: I18n.i18nc("@info:shortcut key name", "Alt+Enter"), action: I18n.i18nc("@info:shortcut action", "Show in Queue") }
        ]
        const all = pairs.concat([close])
        return root.hintLines(all) > 1 && root.hintLines(pairs) === 1 ? pairs : all
    }
    readonly property real footerHintsWidth: root.width - root.titleX - root.sideInset
    function hintPairWidth(hint, last) {
        return Math.ceil(smallMetrics.advanceWidth(hint.key)) + root.hintKeyGap
            + Math.ceil(smallMetrics.advanceWidth(hint.action)) + (last ? 0 : root.hintPairGap)
    }
    function hintLines(hints) {
        let lines = hints.length > 0 ? 1 : 0
        let x = 0
        for (let i = 0; i < hints.length; ++i) {
            const width = root.hintPairWidth(hints[i], i === hints.length - 1)
            if (x > 0 && x + width > root.footerHintsWidth) {
                lines += 1
                x = 0
            }
            x += width
        }
        return lines
    }
    function footerHeightFor(hints) {
        return Math.max(28, root.hintLines(hints) * root.footerLineHeight + 8)
    }

    contentItem: Item {
        id: contentColumn
        implicitWidth: root.designWidth
        implicitHeight: root.fieldHeight + 1 + body.implicitHeight + 1 + root.footerHeight

        Item {
            id: fieldRow
            width: parent.width
            height: root.fieldHeight

            // Over the results' lead column, as global search's glyph.
            Kirigami.Icon {
                objectName: "queueSearchFieldIcon"
                x: root.leadX + (root.leadSize - width) / 2
                y: Math.round((parent.height - height) / 2)
                width: root.glyphSize
                height: root.glyphSize
                source: "search"
                isMask: true
                roundToIconSize: false
                color: root.uiPalette.textSecondary
            }
            TextField {
                id: queryField
                objectName: "queueSearchQueryField"
                x: root.titleX
                width: Math.max(0, (clearButton.visible ? clearButton.x : helpButton.x) - x - 4)
                height: parent.height
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                verticalAlignment: TextInput.AlignVCenter
                background: null
                font.family: root.uiPalette.baseFont.family
                font.pointSize: root.uiPalette.basePointSize
                color: root.uiPalette.text
                placeholderTextColor: root.uiPalette.textSecondary
                placeholderText: I18n.i18nc("@info:placeholder", "Find in the queue by title, artist, album, tags or file name")
                Accessible.name: I18n.i18nc("@label accessible name of the search field", "Find in Queue")
                onTextChanged: {
                    root.pendingAction = ""
                    if (root.opened) root.uiBridge.setQueueSearchQuery(text)
                }
                Keys.onPressed: function(event) { root.handleResultKeys(event) }
            }
            ToolButton {
                id: clearButton
                objectName: "queueSearchClearButton"
                x: helpButton.x - width
                y: Math.round((parent.height - height) / 2)
                width: root.fieldButtonSize
                height: root.fieldButtonSize
                visible: queryField.text.length > 0
                display: AbstractButton.IconOnly
                icon.name: "edit-clear"
                icon.width: root.glyphSize
                icon.height: root.glyphSize
                icon.color: clearButton.hovered || clearButton.visualFocus
                    ? root.uiPalette.textSecondary : root.clearGlyphRest
                text: I18n.i18nc("@action:button clear the search field", "Clear")
                Accessible.name: text
                ToolTip.visible: hovered || visualFocus
                ToolTip.delay: Kirigami.Units.toolTipDelay
                ToolTip.text: text
                onClicked: {
                    queryField.clear()
                    root.focusQueryField(false)
                }
            }
            ToolButton {
                id: helpButton
                objectName: "queueSearchHelpButton"
                x: parent.width - 8 - width
                y: Math.round((parent.height - height) / 2)
                width: root.fieldButtonSize
                height: root.fieldButtonSize
                display: AbstractButton.IconOnly
                icon.name: "question"
                icon.width: root.glyphSize
                icon.height: root.glyphSize
                icon.color: root.uiPalette.textSecondary
                text: I18n.i18nc("@action:button", "Search Syntax and Keys")
                Accessible.name: text
                ToolTip.visible: hovered || visualFocus
                ToolTip.delay: Kirigami.Units.toolTipDelay
                ToolTip.text: text
                onClicked: searchHelp.open()
            }
        }
        Rectangle {
            objectName: "queueSearchFieldSeam"
            y: root.fieldHeight
            width: parent.width
            height: 1
            color: root.uiPalette.seam
        }

        Item {
            id: body
            objectName: "queueSearchBody"
            y: root.fieldHeight + 1
            width: parent.width
            height: Math.max(0, contentColumn.height - y - 1 - root.footerHeight)
            clip: true
            // As many whole rows as fit, and 6 px under the last where they
            // all fit; the rows are alike in height.
            readonly property real resultsHeight: {
                const room = root.maximumHeight - root.fieldHeight - 2 - root.footerHeight
                const all = root.rowCount * root.resultHeight
                if (all + resultsView.bottomMargin <= room) return all + resultsView.bottomMargin
                return Math.max(0, Math.floor(room / root.resultHeight) * root.resultHeight)
            }
            implicitHeight: root.resultsShown
                ? body.resultsHeight
                : (noResultsMessage.visible ? noResultsMessage.y + noResultsMessage.height + root.messageGap : 0)

            ListView {
                id: resultsView
                objectName: "queueSearchResultsView"
                width: parent.width
                height: parent.height
                visible: root.resultsShown
                clip: true
                model: root.resultsModel || []
                reuseItems: true
                cacheBuffer: 0
                bottomMargin: 6
                currentIndex: root.selectedIndex
                highlightFollowsCurrentItem: false
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                // The rows a running search is to replace take no clicks or
                // scrolling, and dim once it has run for a while, as global
                // search's.
                interactive: !root.searching
                readonly property string runningQuery: root.searching ? root.queryText : ""
                onRunningQueryChanged: {
                    if (resultsView.runningQuery.length === 0) {
                        slowSearchDim.stop()
                        resultsView.opacity = 1
                    } else if (resultsView.opacity === 1) {
                        slowSearchDim.restart()
                    }
                }
                SequentialAnimation {
                    id: slowSearchDim
                    objectName: "queueSearchSlowSearchDim"
                    PauseAnimation { duration: 250 }
                    NumberAnimation {
                        target: resultsView
                        property: "opacity"
                        to: 0.5
                        duration: Kirigami.Units.shortDuration
                        easing.type: Easing.OutCubic
                    }
                }
                ScrollBar.vertical: Components.LaneScrollBar {
                    id: resultScrollBar
                    objectName: "queueSearchScrollBar"
                    uiPalette: root.uiPalette
                    interactive: !root.searching
                }
                readonly property real scrollbarLane: resultScrollBar.visible ? resultScrollBar.lane : 0
                readonly property real reservedLane: resultScrollBar.lane
                Keys.onPressed: function(event) {
                    if (root.handleResultKeys(event)) return
                    if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                        root.selectIndex(event.key === Qt.Key_Home ? 0 : root.rowCount - 1)
                        event.accepted = true
                    }
                }
                delegate: Item {
                    id: resultRow
                    objectName: "queueSearchResultRow"
                    required property int index
                    required property var model
                    // Read as properties of their own: a pooled row keeps
                    // them, where `model` reads undefined (#772).
                    required property string coverUrl
                    required property int queueRow
                    required property int playNext
                    readonly property bool selected: index === root.selectedIndex
                    readonly property bool hovered: rowMouse.containsMouse
                    // The row that plays or is paused now: its own row, not
                    // another row of its file.
                    readonly property bool isNow: queueRow === root.uiBridge.playingQueueIndex
                        && (root.uiBridge.playbackState || "Stopped") !== "Stopped"
                    readonly property var titleRuns: root.titleRuns(model.title, isNow)
                    readonly property color titleColor: !isNow ? root.uiPalette.text
                        : (selected ? root.uiPalette.nowSelected : root.uiPalette.now)
                    readonly property color secondaryColor: selected
                        ? root.uiPalette.textSecondarySelected : root.uiPalette.textSecondary
                    readonly property string context: root.rowContext(model)
                    readonly property string playNextText: playNext > 0
                        ? I18n.i18nc("@info accessible state of a queue row marked to play next, %1 is its place in their order",
                                     "plays next, number %1", playNext)
                        : ""
                    readonly property string playbackStateText: !isNow ? ""
                        : ((root.uiBridge.playbackState || "") === "Paused"
                            ? I18n.i18nc("@info accessible state of the current queue row", "paused")
                            : I18n.i18nc("@info accessible state of the current queue row", "playing"))
                    width: ListView.view.width
                    height: root.resultHeight
                    Accessible.role: Accessible.ListItem
                    Accessible.name: [
                        I18n.i18nc("@info accessible place of a queue row", "Number %1", root.placeText(queueRow)),
                        model.title || "",
                        resultRow.context,
                        model.lengthText || "",
                        playbackStateText,
                        playNextText
                    ].filter(function(part) { return part.length > 0 }).join(", ")
                    Accessible.selectable: true
                    Accessible.selected: selected

                    Components.RowBackground {
                        uiPalette: root.uiPalette
                        hovered: resultRow.hovered
                        selected: resultRow.selected
                        active: root.selectionActive
                        focused: false
                        scrollbarLane: resultsView.scrollbarLane
                    }
                    // The row's place in the queue, in the queue's tertiary
                    // ink and tabular figures.
                    Text {
                        objectName: "queueSearchResultPlace"
                        x: root.sideInset
                        width: root.placeWidth
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        textFormat: Text.PlainText
                        text: root.placeText(resultRow.queueRow)
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        font.features: root.uiPalette.tabularFigures
                        color: root.uiPalette.textTertiaryOn(resultRow.selected, resultRow.hovered)
                    }
                    Loader {
                        x: root.leadX
                        y: Math.round((parent.height - root.leadSize) / 2)
                        active: resultRow.coverUrl.length === 0 || thumbnail.failed
                        sourceComponent: noCoverTile
                    }
                    Kirigami.ShadowedImage {
                        id: thumbnail
                        objectName: "queueSearchThumbnail"
                        readonly property bool failed: status === Image.Error
                        visible: resultRow.coverUrl.length > 0
                        x: root.leadX
                        y: Math.round((parent.height - height) / 2)
                        width: root.leadSize
                        height: root.leadSize
                        radius: 2
                        color: status === Image.Loading || status === Image.Null
                            ? root.uiPalette.hover : ColorUtils.withAlpha(root.uiPalette.hover, 0)
                        source: visible ? resultRow.coverUrl : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: Math.ceil(root.leadSize * Screen.devicePixelRatio)
                        sourceSize.height: Math.ceil(root.leadSize * Screen.devicePixelRatio)
                    }
                    Rectangle {
                        objectName: "queueSearchNowScrim"
                        visible: resultRow.isNow
                        x: root.leadX
                        y: thumbnail.y
                        width: root.leadSize
                        height: root.leadSize
                        radius: 2
                        color: Qt.rgba(0, 0, 0, 0.55)
                    }
                    Loader {
                        x: root.leadX + (root.leadSize - root.nowGlyphSize) / 2
                        y: Math.round((parent.height - root.nowGlyphSize) / 2)
                        active: resultRow.isNow
                        sourceComponent: Kirigami.Icon {
                            objectName: "queueSearchNowGlyph"
                            width: root.nowGlyphSize
                            height: root.nowGlyphSize
                            source: (root.uiBridge.playbackState || "") === "Paused"
                                ? "media-playback-pause" : "media-playback-start"
                            isMask: true
                            roundToIconSize: false
                            color: root.nowOnScrim
                        }
                    }
                    TitleRun { run: resultRow.titleRuns.length > 0 ? resultRow.titleRuns[0] : null; color: resultRow.titleColor }
                    TitleRun { run: resultRow.titleRuns.length > 1 ? resultRow.titleRuns[1] : null; color: resultRow.titleColor }
                    TitleRun { run: resultRow.titleRuns.length > 2 ? resultRow.titleRuns[2] : null; color: resultRow.titleColor }
                    TitleRun { run: resultRow.titleRuns.length > 3 ? resultRow.titleRuns[3] : null; color: resultRow.titleColor }
                    TitleRun { run: resultRow.titleRuns.length > 4 ? resultRow.titleRuns[4] : null; color: resultRow.titleColor }
                    Text {
                        id: resultContext
                        objectName: "queueSearchResultContext"
                        x: root.contextX
                        width: Math.max(0, (playNextPlace.visible ? playNextPlace.x : lengthText.x) - root.columnGap - x)
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        textFormat: Text.PlainText
                        text: resultRow.context
                        elide: Text.ElideRight
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        color: resultRow.secondaryColor
                    }
                    // The row's place among the rows marked to play next, in
                    // the accent, as the queue's row shows it.
                    Text {
                        id: playNextPlace
                        objectName: "queueSearchResultPlayNext"
                        visible: resultRow.playNext > 0
                        x: lengthText.x - root.columnGap - width
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        textFormat: Text.PlainText
                        text: String(resultRow.playNext)
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        font.weight: Font.DemiBold
                        font.features: root.uiPalette.tabularFigures
                        color: resultRow.selected ? root.uiPalette.playNextSelected : root.uiPalette.playNext
                    }
                    Text {
                        id: lengthText
                        objectName: "queueSearchResultLength"
                        x: parent.width - root.sideInset - resultsView.reservedLane - width
                        width: Math.max(root.lengthWidth, Math.ceil(implicitWidth))
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        textFormat: Text.PlainText
                        text: resultRow.model.lengthText || ""
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        font.features: root.uiPalette.tabularFigures
                        color: resultRow.secondaryColor
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onPressed: function(mouse) {
                            mouse.accepted = !root.searching
                        }
                        onClicked: function(mouse) {
                            if (root.searching) return
                            root.selectedIndex = resultRow.index
                            if (mouse.button === Qt.RightButton) {
                                resultMenu.popupAt(rowMouse, mouse.x, mouse.y)
                            } else {
                                resultsView.forceActiveFocus()
                            }
                        }
                        onDoubleClicked: function(mouse) {
                            if (mouse.button === Qt.LeftButton && !root.searching) root.playSelection()
                        }
                    }
                }
            }

            Item {
                id: noResultsMessage
                objectName: "queueSearchNoResults"
                visible: root.noResults
                y: root.messageGap
                width: parent.width
                height: root.resultHeight + root.hintHeight
                Text {
                    objectName: "queueSearchNoResultsTitle"
                    x: root.titleX
                    width: parent.width - x - root.sideInset
                    height: root.resultHeight
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: I18n.i18nc("@info", "No matches for “%1” in the queue", root.queryText)
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.basePointSize
                    color: root.uiPalette.text
                }
                Text {
                    objectName: "queueSearchNoResultsHint"
                    x: root.titleX
                    y: root.resultHeight - 6
                    width: parent.width - x - root.sideInset
                    height: root.hintHeight
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: I18n.i18nc("@info", "Check the spelling, or try fewer words.")
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.smallPointSize
                    color: root.uiPalette.textSecondary
                }
            }
        }

        Rectangle {
            objectName: "queueSearchFooterHairline"
            visible: body.height > 0
            y: footerBar.y - 1
            width: parent.width
            height: 1
            color: root.uiPalette.hairline
        }
        Item {
            id: footerBar
            objectName: "queueSearchFooter"
            y: parent.height - height
            width: parent.width
            height: root.footerHeight
            Flow {
                id: footerHints
                x: root.titleX
                y: Math.round((parent.height - height) / 2)
                width: parent.width - x - root.sideInset
                Repeater {
                    model: root.keyHints
                    delegate: Row {
                        required property var modelData
                        required property int index
                        objectName: "queueSearchKeyHint"
                        readonly property string keyText: modelData.key
                        readonly property string actionText: modelData.action
                        height: root.footerLineHeight
                        spacing: root.hintKeyGap
                        Text {
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            textFormat: Text.PlainText
                            text: modelData.key
                            font.family: root.uiPalette.baseFont.family
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.text
                        }
                        Text {
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            textFormat: Text.PlainText
                            text: modelData.action
                            font.family: root.uiPalette.baseFont.family
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.textSecondary
                        }
                        Item {
                            visible: index < root.keyHints.length - 1
                            width: root.hintPairGap - parent.spacing
                            height: 1
                        }
                    }
                }
            }
        }

        Popup {
            id: searchHelp
            objectName: "queueSearchHelp"
            focus: true
            width: Math.min(root.uiPalette.scaledSize(480), root.width - 2 * root.sideInset)
            x: root.width - width - 8
            y: root.fieldHeight
            padding: 12
            contentItem: Label {
                // The filter names are search syntax, the same in every language.
                textFormat: Text.PlainText
                text: I18n.i18nc("@info search help", "Finds tracks in the queue as global search finds them. Use \"quotes\" for phrases. Combine words and filters to narrow results.")
                    + '\n\ntitle: · artist: · album: · albumartist: · genre: · year: · date:\n'
                    + 'composer: · conductor: · performer: · label: · comment: · lyrics:\n'
                    + 'root: · path: · track: · disc:\n\n'
                    + I18n.i18nc("@info search help",
                        "Up/Down: select · Page Up/Down: move by page\nEnter: play · Ctrl+Enter: play next · Alt+Enter: show in the queue\nCtrl+F: edit the query · Esc: close")
                wrapMode: Text.Wrap
            }
        }
        // The Menu key and Shift+F10 open the selected result's menu under
        // its row.
        Shortcut {
            sequences: ["Menu", "Shift+F10"]
            context: Qt.WindowShortcut
            enabled: root.opened && root.resultsShown && !root.searching && !resultMenu.visible
                && !!root.selectedRow
            onActivated: {
                resultsView.positionViewAtIndex(root.selectedIndex, ListView.Contain)
                const row = resultsView.itemAtIndex(root.selectedIndex)
                if (row) resultMenu.popupAt(row, 0, row.height)
                else resultMenu.popupAt(resultsView, 0, 0)
            }
        }
        Components.AppMenu {
            id: resultMenu
            objectName: "queueSearchResultMenu"

            Components.KeyHintMenuItem {
                objectName: "queueSearchPlayItem"
                text: I18n.i18nc("@action:inmenu", "Play")
                icon.name: "media-playback-start"
                keyHint: I18n.i18nc("@info:shortcut key name", "Enter")
                onTriggered: root.playSelection()
            }
            Components.KeyHintMenuItem {
                objectName: "queueSearchPlayNextItem"
                text: root.selectedPlaysNext ? I18n.i18nc("@action:inmenu", "Don’t Play Next")
                                             : I18n.i18nc("@action:inmenu", "Play Next")
                icon.name: "go-next"
                keyHint: I18n.i18nc("@info:shortcut key name", "Ctrl+Enter")
                onTriggered: root.togglePlayNextOfSelection()
            }
            MenuSeparator {}
            Components.KeyHintMenuItem {
                objectName: "queueSearchShowItem"
                text: I18n.i18nc("@action:inmenu", "Show in Queue")
                icon.name: "view-media-playlist"
                keyHint: I18n.i18nc("@info:shortcut key name", "Alt+Enter")
                onTriggered: root.showSelectionInQueue()
            }
        }
    }

    // A run of a result's title: the matches semibold.
    component TitleRun: Text {
        property var run: null
        objectName: "queueSearchResultTitle"
        visible: !!run && run.width > 0
        x: root.titleX + (run ? run.x : 0)
        width: run ? run.width : 0
        height: root.resultHeight
        verticalAlignment: Text.AlignVCenter
        text: run ? run.text : ""
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
        font.weight: run && run.match ? Font.DemiBold : Font.Normal
    }
    Component {
        id: noCoverTile
        Components.NoCoverTile {
            objectName: "queueSearchCoverPlaceholder"
            uiPalette: root.uiPalette
            size: root.leadSize
            radius: 2
        }
    }
}
