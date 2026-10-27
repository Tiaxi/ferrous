// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components
import "../logic/KeyUtils.js" as KeyUtils

Rectangle {
    id: root

    required property var controller
    required property var uiBridge
    required property var uiPalette
    required property real preferredHeight
    required property int playlistIndicatorColumnWidth
    required property int playlistOrderColumnWidth
    required property var playlistOrderText
    required property var libraryController
    required property var stepScrollView
    required property var clearPlaylistAction
    required property int popupTransitionMs
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property var droppedExternalPaths

    // The columns, which the header and the rows share; text that does not
    // fit its column is elided, so it never runs into the next. The title
    // takes the width the others leave. Artist, Album and Length have the widths the
    // user dragged their handles to, which the settings keep, or defaults
    // that follow the font size.
    readonly property real columnMargin: 8
    readonly property real columnSpacing: 6
    readonly property real defaultArtistColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 25)
    readonly property real defaultAlbumColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 28)
    readonly property real defaultLengthColumnWidth: Math.ceil(Math.max(
        lengthHeaderMetrics.advanceWidth,
        lengthValueMetrics.advanceWidth) + columnFontMetrics.averageCharacterWidth)
    readonly property real artistColumnWidth: root.columnWidths.artist
    readonly property real albumColumnWidth: root.columnWidths.album
    readonly property real lengthColumnWidth: root.columnWidths.length
    readonly property var resizableColumns: ["artist", "album", "length"]
    // The narrowest Artist and Album can be dragged: a few characters, and
    // no narrower than the backend keeps. Length keeps room for its
    // heading and the longest length it shows.
    readonly property real minimumResizedColumnWidth: Math.max(
        16, Math.ceil(columnFontMetrics.averageCharacterWidth * 4))
    readonly property real minimumLengthColumnWidth: Math.max(16, Math.ceil(Math.max(
        lengthHeaderMetrics.advanceWidth,
        lengthValueMetrics.advanceWidth)))
    // The widest a column is kept, as wide as the backend saves:
    // BinaryBridgeCodec::kMaxQueueColumnWidth.
    readonly property real maximumResizedColumnWidth: 4096
    // The width Artist, Album and Length share, which leaves the title its
    // minimum.
    readonly property real resizableColumnsRoom: Math.max(0, queueHeaderRow.width
        - root.playlistIndicatorColumnWidth - root.playlistOrderColumnWidth
        - root.minimumTitleColumnWidth - 5 * root.columnSpacing)
    // The column whose handle is being dragged, or moved with the keys; its
    // preferred width until the drag ends and the width is kept; and, from
    // when the drag started, its preferred and shown widths, how far the
    // drag has moved its edge, and whether the columns were giving way.
    property string resizingColumn: ""
    property real resizingWidth: 0
    property real resizeStartWidth: 0
    property real resizeStartShownWidth: 0
    property real resizeDelta: 0
    property bool resizeStartedCompressed: false

    function defaultColumnWidth(column) {
        if (column === "artist") {
            return root.defaultArtistColumnWidth
        }
        if (column === "album") {
            return root.defaultAlbumColumnWidth
        }
        return root.defaultLengthColumnWidth
    }

    function minimumColumnWidth(column) {
        return column === "length" ? root.minimumLengthColumnWidth : root.minimumResizedColumnWidth
    }

    function clampColumnWidth(column, width) {
        return Math.max(root.minimumColumnWidth(column),
                        Math.min(root.maximumResizedColumnWidth, width))
    }

    // The width the column would have with room enough: the one the user
    // gave it, or its default.
    function savedColumnWidth(column) {
        const saved = root.uiBridge.queueColumnWidths
            ? Number(root.uiBridge.queueColumnWidths[column])
            : 0
        return saved > 0 ? root.clampColumnWidth(column, saved) : root.defaultColumnWidth(column)
    }

    function preferredColumnWidth(column) {
        return root.resizingColumn === column
            ? root.resizingWidth
            : root.savedColumnWidth(column)
    }

    // The widths the columns are shown at for `preferred` widths. When the
    // queue is too narrow for them with the title at its minimum, as in a
    // window made smaller after a column was widened, each gives up a share
    // of what it has beyond its minimum.
    function fittedColumnWidths(preferred) {
        const minimum = root.resizableColumns.map(root.minimumColumnWidth)
        const room = root.resizableColumnsRoom
        const total = preferred.reduce((sum, width) => sum + width, 0)
        if (room <= 0 || total <= room) {
            return preferred
        }
        const slack = preferred.map((width, i) => Math.max(0, width - minimum[i]))
        const totalSlack = slack.reduce((sum, width) => sum + width, 0)
        if (totalSlack <= 0) {
            return preferred
        }
        const overflow = Math.min(total - room, totalSlack)
        return preferred.map((width, i) => width - overflow * slack[i] / totalSlack)
    }

    readonly property var columnWidths: {
        const widths = root.fittedColumnWidths(root.resizableColumns.map(root.preferredColumnWidth))
        return {
            "artist": Math.round(widths[0]),
            "album": Math.round(widths[1]),
            "length": Math.round(widths[2])
        }
    }

    // The preferred width at which `column` is shown `shown` wide, the
    // others keeping theirs: fittedColumnWidths the other way round. Where
    // the columns give way, the shown width is m + A u / (u + S), for the
    // column's minimum m and preferred width m + u, the others' S beyond
    // their minimums, and the room A beyond all the minimums.
    function preferredWidthShowing(column, shown) {
        let others = 0
        let othersSlack = 0
        let minimums = 0
        for (const name of root.resizableColumns) {
            const minimum = root.minimumColumnWidth(name)
            minimums += minimum
            if (name !== column) {
                const width = root.savedColumnWidth(name)
                others += width
                othersSlack += Math.max(0, width - minimum)
            }
        }
        const room = root.resizableColumnsRoom
        if (room <= 0 || shown + others <= room || othersSlack <= 0) {
            return shown
        }
        const beyond = shown - root.minimumColumnWidth(column)
        const roomBeyond = room - minimums
        if (beyond >= roomBeyond) {
            return root.maximumResizedColumnWidth
        }
        return root.minimumColumnWidth(column) + othersSlack * beyond / (roomBeyond - beyond)
    }

    function beginColumnResize(column) {
        if (root.resizingColumn !== column) {
            root.finishColumnResize()
            // Read before the column follows resizingWidth.
            const preferred = root.savedColumnWidth(column)
            root.resizeStartShownWidth = root.columnWidths[column]
            root.resizeStartedCompressed = root.resizableColumns
                .map(root.savedColumnWidth)
                .reduce((sum, width) => sum + width, 0) > root.resizableColumnsRoom
            root.resizeStartWidth = preferred
            root.resizingWidth = preferred
            root.resizeDelta = 0
            root.resizingColumn = column
        }
    }

    // Move the column's edge `delta` from where the drag started, as it is
    // shown: the preferred width changes so the shown one follows. A drag
    // that started with room enough stops where the title reaches its
    // minimum; one where the columns gave way takes from the others.
    function resizeColumnBy(column, delta) {
        root.beginColumnResize(column)
        root.resizeDelta = delta
        if (delta === 0) {
            root.resizingWidth = root.resizeStartWidth
            return
        }
        let shown = Math.max(root.minimumColumnWidth(column), root.resizeStartShownWidth + delta)
        if (!root.resizeStartedCompressed) {
            let others = 0
            for (const name of root.resizableColumns) {
                if (name !== column) {
                    others += root.savedColumnWidth(name)
                }
            }
            shown = Math.min(shown, Math.max(root.minimumColumnWidth(column),
                                             root.resizableColumnsRoom - others))
        }
        root.resizingWidth = Math.round(root.clampColumnWidth(
            column, root.preferredWidthShowing(column, shown)))
    }

    // Keep the width the drag ended at, once per drag, if it changed.
    function finishColumnResize() {
        const column = root.resizingColumn
        if (column === "") {
            return
        }
        const width = Math.round(root.resizingWidth)
        if (width !== Math.round(root.resizeStartWidth)) {
            root.uiBridge.setQueueColumnWidth(column, width)
        }
        root.resizingColumn = ""
    }

    function cancelColumnResize() {
        root.resizingColumn = ""
    }

    function restoreDefaultColumnWidth(column) {
        root.cancelColumnResize()
        root.uiBridge.setQueueColumnWidth(column, 0)
    }
    // The narrowest the queue gets: every column, and a title column wide
    // enough to tell the tracks apart. The number column is counted for
    // queues of up to 99,999 tracks rather than at its width now, so that
    // the window's minimum does not change as tracks are queued, and the
    // default widths of the others, so that it does not change as they are
    // resized either: narrower windows shrink resized columns.
    readonly property real minimumTitleColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 12)
    readonly property real minimumOrderColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 6.5)
    readonly property real minimumContentWidth: Math.ceil(2 * root.columnMargin
        + root.playlistIndicatorColumnWidth + root.minimumOrderColumnWidth
        + root.minimumTitleColumnWidth + root.defaultArtistColumnWidth
        + root.defaultAlbumColumnWidth + root.defaultLengthColumnWidth + 5 * root.columnSpacing
        + root.scrollBarAllowance)
    // Room for the scroll bar beside the rows, which some styles widen
    // while it is used; see reservedRightPadding.
    readonly property real scrollBarAllowance: 20

    FontMetrics {
        id: columnFontMetrics
        font: root.uiPalette.baseFont
    }

    TextMetrics {
        id: lengthHeaderMetrics
        font: root.uiPalette.baseFont
        text: "Length"
    }

    TextMetrics {
        id: lengthValueMetrics
        font: root.uiPalette.baseFont
        text: "00:00:00"
    }

    color: root.uiPalette.uiSurfaceRaisedColor
    SplitView.fillWidth: true
    SplitView.preferredHeight: root.preferredHeight
    SplitView.minimumHeight: 220

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.uiPalette.rowHeight + 2
            color: root.uiPalette.uiHeaderColor

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: root.uiPalette.uiBorderColor
            }

            RowLayout {
                id: queueHeaderRow
                anchors.fill: parent
                anchors.leftMargin: root.columnMargin
                anchors.rightMargin: root.columnMargin
                    + (playlistView ? playlistView.reservedRightPadding : 0)
                spacing: root.columnSpacing

                Label {
                    objectName: "queueHeader_indicator"
                    elide: Text.ElideRight
                    text: "▶"
                    Layout.preferredWidth: root.playlistIndicatorColumnWidth
                    horizontalAlignment: Text.AlignHCenter
                    color: root.uiPalette.uiMutedTextColor
                    Accessible.name: "Playing"
                }

                Label {
                    objectName: "queueHeader_order"
                    elide: Text.ElideRight
                    text: "#"
                    Layout.preferredWidth: root.playlistOrderColumnWidth
                    horizontalAlignment: Text.AlignRight
                    color: root.uiPalette.uiMutedTextColor
                    Accessible.name: "Number"
                }

                Label {
                    objectName: "queueHeader_title"
                    elide: Text.ElideRight
                    text: "Title"
                    Layout.fillWidth: true
                    color: root.uiPalette.uiMutedTextColor
                }
                Label {
                    id: artistHeader
                    objectName: "queueHeader_artist"
                    text: "Artist"
                    Layout.preferredWidth: root.artistColumnWidth
                    elide: Text.ElideRight
                    color: root.uiPalette.uiMutedTextColor
                }
                Label {
                    id: albumHeader
                    objectName: "queueHeader_album"
                    text: "Album"
                    Layout.preferredWidth: root.albumColumnWidth
                    elide: Text.ElideRight
                    color: root.uiPalette.uiMutedTextColor
                }
                Label {
                    id: lengthHeader
                    objectName: "queueHeader_length"
                    elide: Text.ElideRight
                    text: "Length"
                    Layout.preferredWidth: root.lengthColumnWidth
                    horizontalAlignment: Text.AlignRight
                    color: root.uiPalette.uiMutedTextColor
                }
            }

            // A handle on the left of each resizable column: the title
            // fills the width the others leave, so dragging it left widens
            // the column after it. Double-click, or Home, restores the
            // column's default width; Left and Right move it from the
            // keyboard.
            Repeater {
                model: [
                    { "column": "artist", "label": "Artist", "header": artistHeader },
                    { "column": "album", "label": "Album", "header": albumHeader },
                    { "column": "length", "label": "Length", "header": lengthHeader }
                ]

                delegate: Item {
                    id: resizeHandle
                    objectName: "queueColumnHandle_" + modelData.column
                    required property var modelData
                    readonly property string column: modelData.column
                    readonly property bool highlighted: resizeMouseArea.containsMouse
                        || resizeMouseArea.pressed || resizeHandle.activeFocus
                    // Where the pointer was when the drag started, in the
                    // pane, which stays put as the columns move.
                    property real pressX: 0

                    x: queueHeaderRow.x + modelData.header.x - root.columnSpacing / 2 - width / 2
                    width: Math.max(8, root.columnSpacing + 2)
                    height: parent.height
                    activeFocusOnTab: true

                    Accessible.role: Accessible.Separator
                    Accessible.name: "Resize the " + modelData.label + " column"
                    Accessible.description: "Left or Right to resize, Home for the default width"
                    Accessible.focusable: true

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        width: resizeHandle.highlighted ? 2 : 1
                        height: parent.height * (resizeHandle.highlighted ? 1 : 0.5)
                        color: resizeHandle.highlighted
                            ? root.uiPalette.uiAccentColor
                            : root.uiPalette.uiBorderColor
                    }

                    MouseArea {
                        id: resizeMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: Qt.SplitHCursor

                        onPressed: function(mouse) {
                            resizeHandle.pressX = mapToItem(root, mouse.x, 0).x
                            root.beginColumnResize(resizeHandle.column)
                        }
                        onPositionChanged: function(mouse) {
                            if (pressed && root.resizingColumn === resizeHandle.column) {
                                const x = mapToItem(root, mouse.x, 0).x
                                root.resizeColumnBy(resizeHandle.column, resizeHandle.pressX - x)
                            }
                        }
                        onReleased: root.finishColumnResize()
                        onCanceled: root.cancelColumnResize()
                        onDoubleClicked: root.restoreDefaultColumnWidth(resizeHandle.column)
                    }

                    function keyResizes(event) {
                        return KeyUtils.isUnmodified(event)
                            && (event.key === Qt.Key_Left || event.key === Qt.Key_Right
                                || event.key === Qt.Key_Home)
                    }
                    // The keys move the focused handle rather than seeking.
                    Keys.onShortcutOverride: function(event) {
                        event.accepted = resizeHandle.keyResizes(event)
                    }
                    Keys.onPressed: function(event) {
                        if (!resizeHandle.keyResizes(event)) {
                            return
                        }
                        event.accepted = true
                        if (event.key === Qt.Key_Home) {
                            root.restoreDefaultColumnWidth(resizeHandle.column)
                            return
                        }
                        const step = Math.max(1, Math.round(columnFontMetrics.averageCharacterWidth))
                        root.beginColumnResize(resizeHandle.column)
                        root.resizeColumnBy(resizeHandle.column,
                            root.resizeDelta + (event.key === Qt.Key_Left ? step : -step))
                    }
                    // A held key moves the handle until it is let go, when
                    // the width is kept.
                    Keys.onReleased: function(event) {
                        if (!event.isAutoRepeat && resizeHandle.keyResizes(event)) {
                            event.accepted = true
                            root.finishColumnResize()
                        }
                    }
                    onActiveFocusChanged: {
                        if (!resizeHandle.activeFocus && !resizeMouseArea.pressed) {
                            root.finishColumnResize()
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: playlistView
                objectName: "playlistView"
                // The controller scrolls and pages by it.
                readonly property real rowHeight: root.uiPalette.rowHeight
                anchors.fill: parent
                clip: true
                activeFocusOnTab: true
                model: root.uiBridge.queueRows
                boundsBehavior: Flickable.StopAtBounds
                boundsMovement: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                pixelAligned: true
                cacheBuffer: 480
                Accessible.role: Accessible.List
                Accessible.name: "Queue"
                property real reservedRightPadding: playlistVerticalScrollBar.visible
                    ? (playlistVerticalScrollBar.width + 6)
                    : 0

                Component.onCompleted: root.controller.registerView(playlistView)

                // The row being dragged, which Escape stops.
                property var draggingRow: null

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape && playlistView.draggingRow) {
                        playlistView.draggingRow.cancelDrag()
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Menu
                            || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                        playlistView.openContextMenuForSelection()
                        event.accepted = true
                        return
                    }
                    root.controller.handlePlaylistKeyPress(event)
                }

                // Opens the context menu from the keyboard, for the selected track.
                function openContextMenuForSelection() {
                    const index = root.controller.primarySelectedIndex()
                    if (index < 0) {
                        return
                    }
                    playlistView.positionViewAtIndex(index, ListView.Contain)
                    const row = playlistView.itemAtIndex(index)
                    playlistContextMenu.rowIndex = index
                    if (row) {
                        playlistContextMenu.popup(row, 0, row.height)
                    } else {
                        playlistContextMenu.popup(playlistView, 0, 0)
                    }
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
                        root.stepScrollView(playlistView, wheel, playlistView.rowHeight, 3)
                    }
                }

                delegate: Rectangle {
                    id: playlistRow
                    objectName: "queueRow"
                    // The row's roles, or its object in a list of objects. A
                    // bare `title` would find the window's title in a list.
                    readonly property var rowValues: model.modelData
                        && typeof model.modelData === "object"
                        ? model.modelData
                        : model
                    readonly property string titleValue: rowValues.title || ""
                    readonly property string artistValue: rowValues.artist || ""
                    readonly property string albumValue: rowValues.album || ""
                    readonly property string lengthTextValue: rowValues.lengthText || "--:--"
                    readonly property bool isCurrentQueueRow: index === root.uiBridge.playingQueueIndex
                    readonly property bool draggableQueueItem: true
                    readonly property int queueRowIndex: index
                    readonly property bool rowSelected: root.controller.isIndexSelected(index)
                    readonly property string playbackStateText: !isCurrentQueueRow
                        ? ""
                        : (root.uiBridge.playbackState === "Paused"
                            ? "paused"
                            : (root.uiBridge.playbackState === "Stopped" ? "stopped" : "playing"))

                    width: Math.max(0, ListView.view.width - (playlistView.reservedRightPadding || 0))
                    height: playlistView.rowHeight
                    color: rowSelected
                        ? root.uiPalette.uiSelectionColor
                        : (index % 2 === 0 ? root.uiPalette.uiSurfaceRaisedColor
                                            : root.uiPalette.uiSurfaceAltColor)

                    Accessible.role: Accessible.ListItem
                    Accessible.name: [
                        root.playlistOrderText(index) + ".",
                        titleValue,
                        artistValue,
                        albumValue,
                        lengthTextValue,
                        playbackStateText
                    ].filter(function(part) { return part.length > 0 }).join(", ")
                    Accessible.selectable: true
                    Accessible.selected: rowSelected

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: root.columnMargin
                        anchors.rightMargin: root.columnMargin
                        spacing: root.columnSpacing

                        Label {
                            objectName: "queueCell_indicator"
                            elide: Text.ElideRight
                            text: {
                                if (!playlistRow.isCurrentQueueRow) {
                                    return ""
                                }
                                if (root.uiBridge.playbackState === "Paused") {
                                    return "⏸"
                                }
                                if (root.uiBridge.playbackState === "Stopped") {
                                    return "■"
                                }
                                return "▶"
                            }
                            Layout.preferredWidth: root.playlistIndicatorColumnWidth
                            horizontalAlignment: Text.AlignHCenter
                            font.bold: true
                            color: playlistRow.rowSelected
                                ? root.uiPalette.uiSelectionTextColor
                                : (playlistRow.isCurrentQueueRow
                                    ? (root.uiBridge.playbackState === "Playing"
                                        ? root.uiPalette.uiActiveIndicatorColor
                                        : root.uiPalette.uiMutedTextColor)
                                    : root.uiPalette.uiTextColor)
                        }

                        Label {
                            objectName: "queueCell_order"
                            elide: Text.ElideRight
                            text: root.playlistOrderText(index)
                            Layout.preferredWidth: root.playlistOrderColumnWidth
                            horizontalAlignment: Text.AlignRight
                            color: playlistRow.rowSelected
                                ? root.uiPalette.uiSelectionTextColor
                                : root.uiPalette.uiTextColor
                        }

                        Label {
                            objectName: "queueCell_title"
                            text: titleValue
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: playlistRow.rowSelected
                                ? root.uiPalette.uiSelectionTextColor
                                : root.uiPalette.uiTextColor
                        }

                        Label {
                            objectName: "queueCell_artist"
                            text: artistValue
                            Layout.preferredWidth: root.artistColumnWidth
                            elide: Text.ElideRight
                            color: playlistRow.rowSelected
                                ? root.uiPalette.uiSelectionTextColor
                                : root.uiPalette.uiTextColor
                        }

                        Label {
                            objectName: "queueCell_album"
                            text: albumValue
                            Layout.preferredWidth: root.albumColumnWidth
                            elide: Text.ElideRight
                            color: playlistRow.rowSelected
                                ? root.uiPalette.uiSelectionTextColor
                                : root.uiPalette.uiTextColor
                        }

                        Label {
                            objectName: "queueCell_length"
                            elide: Text.ElideRight
                            text: lengthTextValue
                            Layout.preferredWidth: root.lengthColumnWidth
                            horizontalAlignment: Text.AlignRight
                            color: playlistRow.rowSelected
                                ? root.uiPalette.uiSelectionTextColor
                                : root.uiPalette.uiTextColor
                        }
                    }

                    MouseArea {
                        id: playlistRowMouseArea
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        drag.target: (pressedButtons & Qt.LeftButton) ? playlistDragProxy : null
                        drag.smoothed: false

                        // Escape stopped this press's drag; the rest of the
                        // press does nothing.
                        property bool dragCancelled: false

                        function endDrag() {
                            playlistDragProxy.x = 0
                            playlistDragProxy.y = 0
                            if (playlistView.draggingRow === playlistRowMouseArea) {
                                playlistView.draggingRow = null
                            }
                        }

                        // Drops nothing and keeps the highlight.
                        function cancelDrag() {
                            dragCancelled = true
                            if (playlistDragProxy.Drag.active) {
                                playlistDragProxy.Drag.cancel()
                            }
                            root.controller.cancelRowPress()
                            endDrag()
                        }

                        onPressed: function(mouse) {
                            dragCancelled = false
                            playlistView.forceActiveFocus()
                            // The proxy starts under the pointer, so it is
                            // where the drag drops.
                            playlistDragProxy.x = mouse.x
                            playlistDragProxy.y = mouse.y
                            root.controller.pressRow(
                                index,
                                mouse.button,
                                mouse.modifiers || Qt.NoModifier)
                        }

                        onPositionChanged: {
                            if (drag.active && !dragCancelled && !playlistDragProxy.Drag.active) {
                                playlistDragProxy.Drag.active = true
                                playlistView.draggingRow = playlistRowMouseArea
                            }
                        }

                        onReleased: function(mouse) {
                            if (dragCancelled) {
                                endDrag()
                                return
                            }
                            const dragged = playlistDragProxy.Drag.active
                            if (dragged) {
                                playlistDragProxy.Drag.drop()
                            }
                            root.controller.releaseRow(index, dragged)
                            endDrag()
                        }

                        onCanceled: {
                            if (playlistDragProxy.Drag.active) {
                                playlistDragProxy.Drag.cancel()
                            }
                            root.controller.cancelRowPress()
                            endDrag()
                        }

                        onClicked: function(mouse) {
                            if (mouse.button === Qt.RightButton) {
                                playlistContextMenu.rowIndex = index
                                playlistContextMenu.popup()
                            }
                        }

                        onDoubleClicked: function(mouse) {
                            if (mouse.button === Qt.LeftButton) {
                                root.controller.playAt(index)
                            }
                        }
                    }

                    // Queue rows drop only on the queue, so they drag within
                    // the window, where the drop area sees each move. The
                    // pointer moves the proxy; the row is what is dropped.
                    Item {
                        id: playlistDragProxy
                        x: 0
                        y: 0
                        width: 1
                        height: 1
                        visible: false
                        Drag.source: playlistRow
                        Drag.supportedActions: Qt.MoveAction
                    }
                }

                Menu {
                    id: playlistContextMenu
                    objectName: "playlistContextMenu"
                    property int rowIndex: -1
                    enter: Components.PopupTransition { duration: root.popupTransitionMs }
                    exit: Components.PopupTransition { duration: root.popupTransitionMs }

                    MenuItem {
                        text: "Play Track"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            if (playlistContextMenu.rowIndex >= 0) {
                                root.controller.playAt(playlistContextMenu.rowIndex)
                            }
                        }
                    }

                    MenuItem {
                        text: "Open containing folder"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            const path = root.uiBridge.queuePathAt(playlistContextMenu.rowIndex)
                            if (path && path.length > 0) {
                                root.uiBridge.openContainingFolder(path)
                            }
                        }
                    }

                    MenuItem {
                        text: "Show in library"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            const path = root.uiBridge.queuePathAt(playlistContextMenu.rowIndex)
                            root.libraryController.showTrackInLibrary(path || "")
                        }
                    }

                    MenuItem {
                        text: "Edit Tags"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: root.controller.openTagEditorForRow(playlistContextMenu.rowIndex)
                    }

                    MenuItem {
                        text: "Remove Track"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            if (playlistContextMenu.rowIndex < 0) {
                                return
                            }
                            if (root.controller.isIndexSelected(playlistContextMenu.rowIndex)
                                    && root.controller.selectedIndices.length > 1) {
                                root.controller.removeSelectedTrack()
                            } else {
                                root.uiBridge.removeAt(playlistContextMenu.rowIndex)
                            }
                        }
                    }

                    MenuSeparator {}

                    MenuItem {
                        text: "Move Up"
                        enabled: playlistContextMenu.rowIndex > 0
                        onTriggered: root.controller.moveRowsOf(playlistContextMenu.rowIndex, -1)
                    }

                    MenuItem {
                        text: "Move Down"
                        enabled: playlistContextMenu.rowIndex >= 0
                            && playlistContextMenu.rowIndex < root.uiBridge.queueLength - 1
                        onTriggered: root.controller.moveRowsOf(playlistContextMenu.rowIndex, 1)
                    }

                    MenuSeparator {}
                    MenuItem { action: root.clearPlaylistAction }
                }
            }

            Label {
                visible: root.uiBridge.queueLength === 0
                text: "Playlist is empty"
                color: Kirigami.Theme.disabledTextColor
                horizontalAlignment: Text.AlignHCenter
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 10
                width: parent.width
            }

            Connections {
                target: root.uiBridge

                function onSnapshotChanged() {
                    if (root.uiBridge.profileLogsEnabled) {
                        const t0 = Date.now()
                        root.controller.handleSnapshotChanged(playlistView)
                        const ms = Date.now() - t0
                        if (ms >= 5)
                            console.warn("[qml-signal-profile] QueuePane.onSnapshotChanged ms=" + ms)
                    } else {
                        root.controller.handleSnapshotChanged(playlistView)
                    }
                }
                function onTrackIdentityChanged() {
                    if (root.uiBridge.profileLogsEnabled) {
                        const t0 = Date.now()
                        root.controller.handleSnapshotChanged(playlistView)
                        const ms = Date.now() - t0
                        if (ms >= 5)
                            console.warn("[qml-signal-profile] QueuePane.onTrackIdentityChanged ms=" + ms)
                    } else {
                        root.controller.handleSnapshotChanged(playlistView)
                    }
                }
            }

            // Queue rows, library rows and files all go where they are
            // dropped, which a line between the rows shows.
            DropArea {
                id: playlistDropArea
                objectName: "playlistDropArea"
                anchors.fill: parent
                property bool queueDropAccepted: false
                property int queueDropInsertIndex: -1
                property real queueDropIndicatorY: 0

                function acceptsDrop(drop) {
                    const src = drop.source
                    if (!src) {
                        return drop.hasUrls
                    }
                    return !!(src.draggableQueueItem || src.draggableLibraryItem)
                }

                // Rows sit from the view's origin on, which edits above a
                // scrolled view move, so a drop is placed in the rows'
                // own coordinates.
                function updateQueueDropIndicator(dropY) {
                    const rowHeight = playlistView.rowHeight
                    const content = playlistView.contentItem
                    const dropContentY = content.mapFromItem(playlistDropArea, 0, dropY).y
                    const insertIndex = root.controller.dropInsertIndex(
                        dropContentY - playlistView.originY, rowHeight)
                    queueDropInsertIndex = insertIndex

                    const contentLineY = playlistView.originY + insertIndex * rowHeight
                    const lineY = playlistDropArea.mapFromItem(content, 0, contentLineY).y
                    const top = playlistDropArea.mapFromItem(playlistView, 0, 0).y
                    queueDropIndicatorY = Math.max(top, Math.min(top + playlistView.height - 2, lineY))
                }

                function finishDrop() {
                    queueDropAccepted = false
                    queueDropInsertIndex = -1
                }

                onEntered: function(drop) {
                    queueDropAccepted = acceptsDrop(drop)
                    if (queueDropAccepted) {
                        updateQueueDropIndicator(drop.y)
                    } else {
                        queueDropInsertIndex = -1
                    }
                }

                onPositionChanged: function(drop) {
                    if (queueDropAccepted) {
                        updateQueueDropIndicator(drop.y)
                    }
                }

                onExited: finishDrop()

                onDropped: function(drop) {
                    updateQueueDropIndicator(drop.y)
                    const insertIndex = queueDropInsertIndex
                    const src = drop.source
                    finishDrop()
                    if (!src) {
                        const externalPaths = root.droppedExternalPaths(drop)
                        if (externalPaths.length > 0) {
                            root.controller.dropExternalPaths(externalPaths, insertIndex)
                            drop.acceptProposedAction()
                        }
                        return
                    }
                    if (src.draggableQueueItem) {
                        root.controller.dropQueueRows(
                            src.queueRowIndex !== undefined ? src.queueRowIndex : -1, insertIndex)
                        drop.acceptProposedAction()
                        return
                    }
                    if (src.draggableLibraryItem) {
                        root.controller.dropLibraryRow(
                            root.libraryController,
                            { selectionKey: src.selectionKeyResolved || "" },
                            insertIndex)
                        drop.acceptProposedAction()
                    }
                }
            }

            Rectangle {
                x: playlistView.x + 4
                width: Math.max(0, playlistView.width - (playlistView.reservedRightPadding || 0) - 8)
                height: 2
                objectName: "queueDropIndicator"
                y: playlistDropArea.queueDropIndicatorY
                radius: 1
                color: Kirigami.Theme.highlightColor
                visible: playlistDropArea.containsDrag
                    && playlistDropArea.queueDropAccepted
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
                    && playlistDropArea.queueDropAccepted
                    && playlistDropArea.queueDropInsertIndex >= 0
            }
        }

        Components.QueueUndoNotice {
            objectName: "queueUndoNotice"
            Layout.fillWidth: true
            Layout.margins: 4
            uiBridge: root.uiBridge
        }
    }
}
