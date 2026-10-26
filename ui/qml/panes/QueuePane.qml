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
    required property var submitExternalImport

    // The columns, which the header and the rows share. The title takes the
    // width the others leave; the others follow the font size.
    readonly property real columnMargin: 8
    readonly property real columnSpacing: 6
    readonly property real artistColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 25)
    readonly property real albumColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 28)
    readonly property real lengthColumnWidth: Math.ceil(Math.max(
        lengthHeaderMetrics.advanceWidth,
        lengthValueMetrics.advanceWidth) + columnFontMetrics.averageCharacterWidth)

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

                Keys.onPressed: function(event) {
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
                    Drag.active: playlistRowMouseArea.drag.active
                    Drag.source: playlistRow
                    Drag.hotSpot.x: width * 0.5
                    Drag.hotSpot.y: height * 0.5
                    Drag.dragType: Drag.Automatic
                    Drag.supportedActions: Qt.MoveAction
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

                        onReleased: {
                            playlistDragProxy.x = 0
                            playlistDragProxy.y = 0
                        }

                        onPressed: function(mouse) {
                            playlistView.forceActiveFocus()
                            root.controller.handleRowSelection(
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
                                root.controller.playAt(index)
                            }
                        }
                    }

                    Item {
                        id: playlistDragProxy
                        x: 0
                        y: 0
                        width: 1
                        height: 1
                        visible: false
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
                        onTriggered: {
                            const from = playlistContextMenu.rowIndex
                            if (from > 0) {
                                root.uiBridge.moveQueue(from, from - 1)
                            }
                        }
                    }

                    MenuItem {
                        text: "Move Down"
                        enabled: playlistContextMenu.rowIndex >= 0
                            && playlistContextMenu.rowIndex < root.uiBridge.queueLength - 1
                        onTriggered: {
                            const from = playlistContextMenu.rowIndex
                            if (from >= 0 && from < root.uiBridge.queueLength - 1) {
                                root.uiBridge.moveQueue(from, from + 1)
                            }
                        }
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

            DropArea {
                id: playlistDropArea
                anchors.fill: parent
                property bool queueReorderDragActive: false
                property int queueDropInsertIndex: -1
                property real queueDropIndicatorY: 0

                function updateQueueDropIndicator(dropY) {
                    const rowHeight = playlistView.rowHeight
                    const yInList = dropY - playlistView.y + playlistView.contentY
                    let insertIndex = Math.floor((yInList + rowHeight * 0.5) / rowHeight)
                    insertIndex = Math.max(0, Math.min(root.uiBridge.queueLength, insertIndex))
                    queueDropInsertIndex = insertIndex

                    const contentLineY = insertIndex * rowHeight
                    const viewLineY = playlistView.y + contentLineY - playlistView.contentY
                    const minY = playlistView.y
                    const maxY = playlistView.y + playlistView.height - 2
                    queueDropIndicatorY = Math.max(minY, Math.min(maxY, viewLineY))
                }

                onEntered: function(drop) {
                    queueReorderDragActive = !!(drop.source && drop.source.draggableQueueItem)
                    if (queueReorderDragActive) {
                        updateQueueDropIndicator(drop.y)
                    } else {
                        queueDropInsertIndex = -1
                    }
                }

                onPositionChanged: function(drop) {
                    if (queueReorderDragActive) {
                        updateQueueDropIndicator(drop.y)
                    }
                }

                onExited: {
                    queueReorderDragActive = false
                    queueDropInsertIndex = -1
                }

                onDropped: function(drop) {
                    const src = drop.source
                    if (!src) {
                        const externalPaths = root.droppedExternalPaths(drop)
                        if (externalPaths.length > 0
                                && root.submitExternalImport(externalPaths, false)) {
                            queueReorderDragActive = false
                            queueDropInsertIndex = -1
                            drop.acceptProposedAction()
                        }
                        return
                    }
                    if (src.draggableQueueItem) {
                        const from = src.queueRowIndex !== undefined ? src.queueRowIndex : -1
                        if (from < 0 || root.uiBridge.queueLength <= 1) {
                            return
                        }
                        let insertIndex = queueDropInsertIndex
                        if (insertIndex < 0) {
                            updateQueueDropIndicator(drop.y)
                            insertIndex = queueDropInsertIndex
                        }
                        let to = insertIndex > from ? insertIndex - 1 : insertIndex
                        to = Math.max(0, Math.min(root.uiBridge.queueLength - 1, to))
                        if (to !== from) {
                            root.uiBridge.moveQueue(from, to)
                        }
                        queueReorderDragActive = false
                        queueDropInsertIndex = -1
                        drop.acceptProposedAction()
                        return
                    }
                    if (!src.draggableLibraryItem) {
                        return
                    }
                    const rowMap = {
                        selectionKey: src.selectionKeyResolved || "",
                        sourceIndex: src.sourceIndexResolved !== undefined ? src.sourceIndexResolved : -1,
                        rowType: src.rowTypeResolved || "",
                        artist: src.artistResolved || "",
                        name: src.nameResolved || "",
                        title: src.rowTitle || "",
                        trackPath: src.trackPathResolved || "",
                        openPath: src.openPathResolved || "",
                        playPaths: src.playPathsResolved || []
                    }
                    const rows = root.libraryController.rowsForAction(rowMap)
                    if (rows.length > 0) {
                        root.libraryController.appendRows(rows)
                        queueReorderDragActive = false
                        queueDropInsertIndex = -1
                        drop.acceptProposedAction()
                    }
                }
            }

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: playlistDropArea.containsDrag
                    && !playlistDropArea.queueReorderDragActive
                    ? 2
                    : 0
                border.color: Kirigami.Theme.highlightColor
                visible: playlistDropArea.containsDrag
                    && !playlistDropArea.queueReorderDragActive
            }

            Rectangle {
                x: playlistView.x + 4
                width: Math.max(0, playlistView.width - (playlistView.reservedRightPadding || 0) - 8)
                height: 2
                y: playlistDropArea.queueDropIndicatorY
                radius: 1
                color: Kirigami.Theme.highlightColor
                visible: playlistDropArea.containsDrag
                    && playlistDropArea.queueReorderDragActive
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
                    && playlistDropArea.queueReorderDragActive
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
