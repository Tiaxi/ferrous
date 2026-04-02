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
    required property var libraryModel
    required property var uiPalette
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property int popupTransitionMs
    required property var stepScrollView
    required property var playAllLibraryTracksAction
    required property var appendAllLibraryTracksAction

    color: root.uiPalette.uiPaneColor

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 6

        Components.TrackMetadataCard {
            Layout.fillWidth: true
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                readonly property int scanBacklog: Math.max(
                    0,
                    root.uiBridge.libraryScanDiscovered - root.uiBridge.libraryScanProcessed)
                text: "Artists: " + root.uiBridge.libraryArtistCount
                      + " | albums: " + root.uiBridge.libraryAlbumCount
                      + " | tracks: " + root.uiBridge.libraryTrackCount
                      + (root.uiBridge.libraryScanInProgress
                          ? (" | scanning " + root.uiBridge.libraryScanProcessed
                             + (scanBacklog > 0 ? (" (+" + scanBacklog + " queued)") : "")
                             + (root.uiBridge.libraryScanFilesPerSecond > 0
                                 ? (" @ " + root.uiBridge.libraryScanFilesPerSecond.toFixed(1) + "/s")
                                 : "")
                             + (root.uiBridge.libraryScanEtaSeconds >= 0
                                 ? (" ETA " + Math.ceil(root.uiBridge.libraryScanEtaSeconds) + "s")
                                 : ""))
                          : "")
                color: Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
            }
        }

        ListView {
            id: libraryAlbumView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.libraryModel
            activeFocusOnTab: true
            focus: true
            reuseItems: true
            cacheBuffer: 200
            boundsBehavior: Flickable.StopAtBounds
            boundsMovement: Flickable.StopAtBounds
            flickDeceleration: root.snappyScrollFlickDeceleration
            maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
            pixelAligned: true

            Component.onCompleted: root.controller.registerView(libraryAlbumView)

            onContentHeightChanged: {
                if (root.controller.pendingExpandFitKey.length > 0) {
                    Qt.callLater(function() {
                        root.controller.applyPendingExpansionFit()
                    })
                }
            }

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AlwaysOn
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                preventStealing: true
                onWheel: function(wheel) {
                    root.stepScrollView(libraryAlbumView, wheel, 24, 3)
                }
            }

            Keys.onPressed: function(event) {
                root.controller.handleKeyPress(event)
            }

            delegate: Rectangle {
                id: libraryRow

                readonly property string rowTypeResolved: rowType || ""
                readonly property bool isAlbumRow: rowTypeResolved === "album"
                readonly property bool isSectionRow: rowTypeResolved === "section"
                readonly property bool isTrackRow: rowTypeResolved === "track"
                readonly property bool hasChildren: !isTrackRow && (count || 0) > 0
                readonly property string selectionKeyResolved: selectionKey || ""
                readonly property string artistResolved: artist || ""
                readonly property string nameResolved: name || ""
                readonly property string trackPathResolved: trackPath || ""
                readonly property string openPathResolved: openPath || ""
                readonly property var playPathsResolved: playPaths || []
                readonly property bool draggableLibraryItem: isTrackRow
                    || rowTypeResolved === "album"
                    || rowTypeResolved === "artist"
                    || playPathsResolved.length > 0
                readonly property string rowTitle: title || name || artist || ""
                readonly property bool albumCoverInViewport: (isAlbumRow || (isSectionRow && (coverPath || "") !== ""))
                    && (y + height >= libraryAlbumView.contentY - 48)
                    && (y <= libraryAlbumView.contentY + libraryAlbumView.height + 48)
                readonly property int sourceIndexResolved: sourceIndex !== undefined ? sourceIndex : -1
                readonly property int depthResolved: depth !== undefined ? depth : 0

                width: ListView.view.width
                height: 24
                color: root.controller.isSelectionKeySelected(selectionKey || "")
                    ? root.uiPalette.uiSelectionColor
                    : (index % 2 === 0
                        ? root.uiPalette.uiSurfaceRaisedColor
                        : root.uiPalette.uiSurfaceAltColor)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 3

                    Item {
                        Layout.preferredWidth: Math.max(0, depthResolved * 18)
                    }

                    Label {
                        id: expanderIcon
                        Layout.preferredWidth: 24
                        Layout.fillHeight: true
                        Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: hasChildren ? (expanded ? "▾" : "▸") : ""
                        font.pixelSize: 20
                        font.bold: true
                        color: root.controller.isSelectionKeySelected(selectionKey || "")
                            ? root.uiPalette.uiSelectionTextColor
                            : root.uiPalette.uiMutedTextColor
                    }

                    Item {
                        visible: isAlbumRow || (isSectionRow && (coverPath || "") !== "")
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        Layout.alignment: Qt.AlignVCenter

                        Image {
                            anchors.fill: parent
                            source: albumCoverInViewport
                                ? root.uiBridge.libraryThumbnailSource(coverPath || "")
                                : ""
                            fillMode: Image.PreserveAspectFit
                            smooth: false
                            asynchronous: true
                            cache: true
                            sourceSize.width: 32
                            sourceSize.height: 32
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.alignment: Qt.AlignVCenter
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        text: rowTitle
                        color: root.controller.isSelectionKeySelected(selectionKey || "")
                            ? root.uiPalette.uiSelectionTextColor
                            : root.uiPalette.uiTextColor
                    }
                }

                Drag.active: libraryRowMouseArea.drag.active && draggableLibraryItem
                Drag.source: libraryRow
                Drag.hotSpot.x: 16
                Drag.hotSpot.y: height * 0.5
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction

                Item {
                    id: libraryDragProxy
                    visible: false
                }

                MouseArea {
                    id: libraryRowMouseArea
                    anchors.fill: parent
                    preventStealing: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    drag.target: draggableLibraryItem ? libraryDragProxy : null
                    drag.smoothed: false

                    onReleased: {
                        libraryDragProxy.x = 0
                        libraryDragProxy.y = 0
                    }

                    onClicked: function(mouse) {
                        libraryAlbumView.forceActiveFocus()
                        const rowMap = {
                            selectionKey: selectionKeyResolved,
                            sourceIndex: sourceIndexResolved,
                            rowType: rowTypeResolved,
                            artist: artist || "",
                            name: name || "",
                            title: rowTitle,
                            trackPath: trackPathResolved,
                            openPath: openPathResolved,
                            playPaths: playPathsResolved
                        }
                        if (mouse.button === Qt.LeftButton
                                && hasChildren
                                && mouse.x <= expanderIcon.x + expanderIcon.width + 6) {
                            root.controller.toggleNode(key)
                            return
                        }
                        root.controller.handleRowSelection(
                            index,
                            rowMap,
                            mouse.button,
                            mouse.modifiers || Qt.NoModifier)
                        if (mouse.button === Qt.RightButton) {
                            libraryContextMenu.rowMap = rowMap
                            libraryContextMenu.popup()
                        }
                    }

                    onDoubleClicked: function(mouse) {
                        const rowMap = {
                            selectionKey: selectionKeyResolved,
                            sourceIndex: sourceIndexResolved,
                            rowType: rowTypeResolved,
                            artist: artist || "",
                            name: name || "",
                            title: rowTitle,
                            trackPath: trackPathResolved,
                            openPath: openPathResolved,
                            playPaths: playPathsResolved
                        }
                        if (hasChildren && mouse.x <= expanderIcon.x + expanderIcon.width + 6) {
                            root.controller.toggleNode(key)
                            return
                        }
                        const rows = root.controller.rowsForAction(rowMap)
                        if (rows.length > 0) {
                            root.controller.playRows(rows)
                        }
                    }
                }

                Menu {
                    id: libraryContextMenu
                    property var rowMap: ({})
                    enter: Components.PopupTransition { duration: root.popupTransitionMs }
                    exit: Components.PopupTransition { duration: root.popupTransitionMs }

                    MenuItem {
                        text: "Play"
                        enabled: root.controller.isActionableRow(libraryContextMenu.rowMap)
                        onTriggered: {
                            const rows = root.controller.rowsForAction(libraryContextMenu.rowMap)
                            if (rows.length > 0) {
                                root.controller.playRows(rows)
                            }
                        }
                    }

                    MenuItem {
                        text: "Queue"
                        enabled: root.controller.isActionableRow(libraryContextMenu.rowMap)
                        onTriggered: {
                            const rows = root.controller.rowsForAction(libraryContextMenu.rowMap)
                            if (rows.length > 0) {
                                root.controller.appendRows(rows)
                            }
                        }
                    }

                    MenuItem {
                        text: "Edit Tags"
                        visible: root.controller.canOpenTagEditorForRow(libraryContextMenu.rowMap)
                        enabled: root.controller.canOpenTagEditorForRow(libraryContextMenu.rowMap)
                        onTriggered: root.controller.openTagEditorForRow(libraryContextMenu.rowMap)
                    }

                    MenuSeparator {}
                    MenuItem { action: root.playAllLibraryTracksAction }
                    MenuItem { action: root.appendAllLibraryTracksAction }
                    MenuSeparator {}

                    MenuItem {
                        text: "Open in " + root.uiBridge.fileBrowserName
                        visible: libraryContextMenu.rowMap.rowType !== "track"
                        enabled: (libraryContextMenu.rowMap.openPath || "").length > 0
                        onTriggered: root.uiBridge.openInFileBrowser(
                            libraryContextMenu.rowMap.openPath || "")
                    }

                    MenuItem {
                        text: "Open containing folder"
                        visible: libraryContextMenu.rowMap.rowType === "track"
                        enabled: (libraryContextMenu.rowMap.trackPath || "").length > 0
                        onTriggered: root.uiBridge.openContainingFolder(
                            libraryContextMenu.rowMap.trackPath || "")
                    }
                }
            }
        }

        Label {
            visible: libraryAlbumView.count === 0
                && libraryAlbumView.contentHeight <= libraryAlbumView.height
            text: root.controller.isTreeLoading() ? "Loading library..." : "Library is empty"
            color: root.uiPalette.uiMutedTextColor
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
