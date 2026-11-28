// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// Find in Queue (#814), in global search's frame
// (Components.SearchPalette), for the queue alone. The results are queue
// rows in queue order, each with its place in the queue; Enter plays the
// row without replacing the queue, Ctrl+Enter marks it to play next, and
// Alt+Enter shows it in the queue. The queue's selection and scroll stay
// as they are until one of them acts.
Components.SearchPalette {
    id: root
    objectName: "queueSearchDialog"
    namePrefix: "queueSearch"
    required property var uiBridge
    // Selects queue row `row` in the queue and shows it there, with the
    // keyboard on the queue.
    required property var showQueueRow
    // Gives the queue the keyboard, once the palette has closed and given
    // back the focus it took.
    property var focusQueue: function() {}
    // Whether the queue takes the keyboard as the palette closes.
    property bool focusQueueOnClose: false

    readonly property var resultsModel: root.uiBridge.queueSearchModel || null
    searching: !!root.uiBridge.queueSearchBusy
    rowCount: root.resultsModel ? (root.resultsModel.count || 0) : 0
    resultsShown: root.queryText.length > 0 && root.rowCount > 0
    noResults: root.queryText.length > 0 && !root.searching && root.rowCount === 0
    highlighter: root.resultsModel
    playbackPaused: (root.uiBridge.playbackState || "") === "Paused"
    resultMenu: resultMenu
    keyHints: root.hintsFor(root.resultsShown && !!root.selectedRow)
    title: I18n.i18nc("@title:window", "Find in Queue")
    fieldPlaceholderText: I18n.i18nc("@info:placeholder", "Find in the queue by title, artist, album, tags or file name")
    fieldAccessibleName: I18n.i18nc("@label accessible name of the search field", "Find in Queue")
    // The filter names are search syntax, the same in every language.
    helpText: I18n.i18nc("@info search help", "Finds tracks in the queue as global search finds them. Use \"quotes\" for phrases. Combine words and filters to narrow results.")
        + '\n\ntitle: · artist: · album: · albumartist: · genre: · year: · date:\n'
        + 'composer: · conductor: · performer: · label: · comment: · lyrics:\n'
        + 'root: · path: · track: · disc:\n\n'
        + I18n.i18nc("@info search help",
            "Up/Down: select · Page Up/Down: move by page\nEnter: play · Ctrl+Enter: play next · Alt+Enter: show in the queue\nCtrl+F: edit the query · Esc: close")
    // The place in the queue before the lead, right-aligned in a column as
    // wide as the queue's largest place, in tabular figures, as the
    // queue's own column; the lead and the title after it.
    readonly property real placeWidth: Math.ceil(placeMetrics.advanceWidth)
    leadX: root.sideInset + root.placeWidth + Kirigami.Units.largeSpacing
    leastFooterHeight: root.footerHeightFor(root.hintsFor(true))
    bodyHeight: root.resultsShown
        ? root.resultsHeight
        : (noResultsMessage.visible ? noResultsMessage.y + noResultsMessage.height + root.messageGap : 0)

    // The selected result; -1 for none.
    property int selectedIndex: -1
    // What Enter, Ctrl+Enter or Alt+Enter pressed while the rows were not
    // yet the results of the query as typed does once they are:
    // "play", "playNext" or "show", as in global search (#723).
    property string pendingAction: ""
    // Counts the rows' changes, for what reads the selected row's data.
    property int rowsRevision: 0
    // As many whole rows as fit, and 6 px under the last where they all
    // fit; the rows are alike in height.
    readonly property real resultsHeight: {
        const room = root.bodyRoom
        const all = root.rowCount * root.resultHeight
        if (all + resultsView.bottomMargin <= room) return all + resultsView.bottomMargin
        return Math.max(0, Math.floor(room / root.resultHeight) * root.resultHeight)
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
    onOpened: {
        // The query kept from the last time is searched again, selected,
        // so typing replaces it.
        root.uiBridge.setQueueSearchQuery(root.queryField.text)
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
    onQueryEdited: {
        root.pendingAction = ""
        if (root.opened) root.uiBridge.setQueueSearchQuery(root.queryField.text)
    }
    onFieldKeyPressed: function(event) { root.handleResultKeys(event) }

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
            root.keptQueueRow = root.resultsQuery === root.queryField.text ? root.selectedQueueRow() : -1
        }
        function onModelReset() {
            root.resultsQuery = root.queryField.text
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
                // The menu of the result removed acts on no other row.
                resultMenu.dismiss()
                root.selectedIndex = Math.min(first, root.resultsModel.rowCount() - 1)
            }
        }
        // A run of results moved with their rows: the selection stays on
        // its result.
        function onRowsMoved(parent, start, end, destination, row) {
            root.rowsRevision += 1
            const index = root.selectedIndex
            if (index < 0) return
            const count = end - start + 1
            const to = row > start ? row - count : row
            if (index >= start && index <= end) {
                root.selectedIndex = to + (index - start)
            } else {
                const rest = index > end ? index - count : index
                root.selectedIndex = rest >= to ? rest + count : rest
            }
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
    // A result's details after its title: who and where it is from, and
    // its genre where the query matched it.
    function rowContext(row) {
        const parts = [[row.artist || "", row.album || ""].filter(function(value) { return value.length > 0 }).join(" — ")]
        if (row.genre && root.queryMatches(row.genre)) parts.push(row.genre)
        return parts.filter(function(value) { return value.length > 0 }).join(root.contextSeparator)
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
    function hintsFor(withResult) {
        if (!withResult) return root.keyHintsWith([])
        return root.keyHintsWith([
            { key: I18n.i18nc("@info:shortcut key name", "Enter"), action: I18n.i18nc("@info:shortcut action", "Play") },
            { key: I18n.i18nc("@info:shortcut key name", "Ctrl+Enter"),
              action: root.selectedPlaysNext ? I18n.i18nc("@info:shortcut action", "Don’t Play Next")
                                             : I18n.i18nc("@info:shortcut action", "Play Next") },
            { key: I18n.i18nc("@info:shortcut key name", "Alt+Enter"), action: I18n.i18nc("@info:shortcut action", "Show in Queue") }
        ])
    }

    Components.SearchPaletteResultsView {
        id: resultsView
        searchPalette: root
        model: root.resultsModel || []
        currentIndex: root.selectedIndex
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
            Components.SearchPaletteCoverLead {
                searchPalette: root
                coverUrl: resultRow.coverUrl
                isNow: resultRow.isNow
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 0 ? resultRow.titleRuns[0] : null
                color: resultRow.titleColor
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 1 ? resultRow.titleRuns[1] : null
                color: resultRow.titleColor
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 2 ? resultRow.titleRuns[2] : null
                color: resultRow.titleColor
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 3 ? resultRow.titleRuns[3] : null
                color: resultRow.titleColor
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 4 ? resultRow.titleRuns[4] : null
                color: resultRow.titleColor
            }
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

    Components.SearchPaletteNoResults {
        id: noResultsMessage
        searchPalette: root
        titleText: I18n.i18nc("@info", "No matches for “%1” in the queue", root.queryText)
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
