// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

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

    // The columns, which the header and the rows share. The title takes the
    // width the others leave; the others follow the font size.
    readonly property real columnMargin: 8
    readonly property real columnSpacing: 6
    readonly property real artistColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 25)
    readonly property real albumColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 28)
    readonly property real lengthColumnWidth: Math.ceil(Math.max(
        lengthHeaderMetrics.advanceWidth,
        lengthValueMetrics.advanceWidth) + columnFontMetrics.averageCharacterWidth)
    // The narrowest the queue gets: every column, and a title column wide
    // enough to tell the tracks apart. The number column is counted for
    // queues of up to 99,999 tracks rather than at its width now, so that
    // the window's minimum does not change as tracks are queued.
    readonly property real minimumTitleColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 12)
    readonly property real minimumOrderColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 6.5)
    readonly property real minimumContentWidth: Math.ceil(2 * root.columnMargin
        + root.playlistIndicatorColumnWidth + root.minimumOrderColumnWidth
        + root.minimumTitleColumnWidth + root.artistColumnWidth + root.albumColumnWidth
        + root.lengthColumnWidth + 5 * root.columnSpacing + root.scrollBarAllowance)
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
                anchors.fill: parent
                anchors.leftMargin: root.columnMargin
                anchors.rightMargin: root.columnMargin
                    + (playlistView ? playlistView.reservedRightPadding : 0)
                spacing: root.columnSpacing

                Label {
                    objectName: "queueHeader_indicator"
                    text: "▶"
                    Layout.preferredWidth: root.playlistIndicatorColumnWidth
                    horizontalAlignment: Text.AlignHCenter
                    color: root.uiPalette.uiMutedTextColor
                    Accessible.name: "Playing"
                }

                Label {
                    objectName: "queueHeader_order"
                    text: "#"
                    Layout.preferredWidth: root.playlistOrderColumnWidth
                    horizontalAlignment: Text.AlignRight
                    color: root.uiPalette.uiMutedTextColor
                    Accessible.name: "Number"
                }

                Label {
                    objectName: "queueHeader_title"
                    text: "Title"
                    Layout.fillWidth: true
                    color: root.uiPalette.uiMutedTextColor
                }
                Label {
                    objectName: "queueHeader_artist"
                    text: "Artist"
                    Layout.preferredWidth: root.artistColumnWidth
                    elide: Text.ElideRight
                    color: root.uiPalette.uiMutedTextColor
                }
                Label {
                    objectName: "queueHeader_album"
                    text: "Album"
                    Layout.preferredWidth: root.albumColumnWidth
                    elide: Text.ElideRight
                    color: root.uiPalette.uiMutedTextColor
                }
                Label {
                    objectName: "queueHeader_length"
                    text: "Length"
                    Layout.preferredWidth: root.lengthColumnWidth
                    horizontalAlignment: Text.AlignRight
                    color: root.uiPalette.uiMutedTextColor
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
