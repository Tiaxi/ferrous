import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

Rectangle {
    id: root

    required property var uiBridge
    required property var palette
    required property real preferredHeight
    required property int playlistIndicatorColumnWidth
    required property int playlistOrderColumnWidth
    required property var playlistOrderText
    required property var isQueueIndexSelected
    required property var handleQueueRowSelection
    required property var openTagEditorForPlaylistRow
    required property var requestPlaylistViewportRestoreWindow
    required property var removeSelectedQueueTrack
    required property var stepScrollView
    required property var handlePlaylistKeyPress
    required property var clearPlaylistAction
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property var selectedQueueIndices

    signal viewReady(var view)

    color: root.palette.uiSurfaceRaisedColor
    SplitView.fillWidth: true
    SplitView.preferredHeight: root.preferredHeight
    SplitView.minimumHeight: 220
    border.color: root.palette.uiBorderColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 26
            color: root.palette.uiHeaderColor
            border.color: root.palette.uiBorderColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8 + (playlistView ? playlistView.reservedRightPadding : 0)

                Label {
                    text: "▶"
                    Layout.preferredWidth: root.playlistIndicatorColumnWidth
                    horizontalAlignment: Text.AlignHCenter
                    color: root.palette.uiMutedTextColor
                }

                Label {
                    text: "#"
                    Layout.preferredWidth: root.playlistOrderColumnWidth
                    horizontalAlignment: Text.AlignRight
                    color: root.palette.uiMutedTextColor
                }

                Label { text: "Title"; Layout.fillWidth: true; color: root.palette.uiMutedTextColor }
                Label { text: "Artist"; Layout.preferredWidth: 170; color: root.palette.uiMutedTextColor }
                Label { text: "Album"; Layout.preferredWidth: 190; color: root.palette.uiMutedTextColor }
                Label {
                    text: "Length"
                    Layout.preferredWidth: 76
                    horizontalAlignment: Text.AlignRight
                    color: root.palette.uiMutedTextColor
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: playlistView
                anchors.fill: parent
                clip: true
                activeFocusOnTab: true
                model: root.uiBridge.queueRows
                boundsBehavior: Flickable.StopAtBounds
                boundsMovement: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                pixelAligned: true
                property real reservedRightPadding: playlistVerticalScrollBar.visible
                    ? (playlistVerticalScrollBar.width + 6)
                    : 0

                Component.onCompleted: root.viewReady(playlistView)

                Keys.onPressed: function(event) {
                    root.handlePlaylistKeyPress(event)
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
                        root.stepScrollView(playlistView, wheel, 24, 3)
                    }
                }

                delegate: Rectangle {
                    id: playlistRow
                    readonly property string titleValue: (typeof title !== "undefined" && title !== undefined)
                        ? title
                        : ((modelData && typeof modelData === "object") ? (modelData.title || "") : "")
                    readonly property string artistValue: (typeof artist !== "undefined" && artist !== undefined)
                        ? artist
                        : ((modelData && typeof modelData === "object") ? (modelData.artist || "") : "")
                    readonly property string albumValue: (typeof album !== "undefined" && album !== undefined)
                        ? album
                        : ((modelData && typeof modelData === "object") ? (modelData.album || "") : "")
                    readonly property string lengthTextValue: (typeof lengthText !== "undefined" && lengthText !== undefined)
                        ? lengthText
                        : ((modelData && typeof modelData === "object") ? (modelData.lengthText || "--:--") : "--:--")
                    readonly property bool isCurrentQueueRow: index === root.uiBridge.playingQueueIndex
                    readonly property bool draggableQueueItem: true
                    readonly property int queueRowIndex: index

                    width: Math.max(0, ListView.view.width - (playlistView.reservedRightPadding || 0))
                    height: 24
                    Drag.active: playlistRowMouseArea.drag.active
                    Drag.source: playlistRow
                    Drag.hotSpot.x: width * 0.5
                    Drag.hotSpot.y: height * 0.5
                    Drag.dragType: Drag.Automatic
                    Drag.supportedActions: Qt.MoveAction
                    color: root.isQueueIndexSelected(index)
                        ? root.palette.uiSelectionColor
                        : (index % 2 === 0 ? root.palette.uiSurfaceRaisedColor
                                            : root.palette.uiSurfaceAltColor)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 6

                        Label {
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
                            color: root.isQueueIndexSelected(index)
                                ? root.palette.uiSelectionTextColor
                                : (playlistRow.isCurrentQueueRow
                                    ? (root.uiBridge.playbackState === "Playing"
                                        ? root.palette.uiActiveIndicatorColor
                                        : root.palette.uiMutedTextColor)
                                    : root.palette.uiTextColor)
                        }

                        Label {
                            text: root.playlistOrderText(index)
                            Layout.preferredWidth: root.playlistOrderColumnWidth
                            horizontalAlignment: Text.AlignRight
                            color: root.isQueueIndexSelected(index)
                                ? root.palette.uiSelectionTextColor
                                : root.palette.uiTextColor
                        }

                        Label {
                            text: titleValue
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: root.isQueueIndexSelected(index)
                                ? root.palette.uiSelectionTextColor
                                : root.palette.uiTextColor
                        }

                        Label {
                            text: artistValue
                            Layout.preferredWidth: 170
                            elide: Text.ElideRight
                            color: root.isQueueIndexSelected(index)
                                ? root.palette.uiSelectionTextColor
                                : root.palette.uiTextColor
                        }

                        Label {
                            text: albumValue
                            Layout.preferredWidth: 190
                            elide: Text.ElideRight
                            color: root.isQueueIndexSelected(index)
                                ? root.palette.uiSelectionTextColor
                                : root.palette.uiTextColor
                        }

                        Label {
                            text: lengthTextValue
                            Layout.preferredWidth: 76
                            horizontalAlignment: Text.AlignRight
                            color: root.isQueueIndexSelected(index)
                                ? root.palette.uiSelectionTextColor
                                : root.palette.uiTextColor
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
                                root.uiBridge.playAt(index)
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
                    property int rowIndex: -1
                    enter: Components.PopupTransition { duration: 0 }
                    exit: Components.PopupTransition { duration: 0 }

                    MenuItem {
                        text: "Play Track"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            if (playlistContextMenu.rowIndex >= 0) {
                                root.uiBridge.playAt(playlistContextMenu.rowIndex)
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
                        text: "Edit Tags"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: root.openTagEditorForPlaylistRow(playlistContextMenu.rowIndex)
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
                                root.requestPlaylistViewportRestoreWindow(700)
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
        }
    }
}
