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
    required property var addLibraryFolderAction

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

            ToolButton {
                text: "Collapse all"
                enabled: root.libraryModel.count > 0
                onClicked: root.controller.collapseAll()
            }
        }

        ListView {
            id: libraryAlbumView
            objectName: "libraryView"
            // The controller pages and scrolls by it.
            readonly property real rowHeight: root.uiPalette.rowHeight
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
            Accessible.role: Accessible.Tree
            Accessible.name: "Library"

            Component.onCompleted: root.controller.registerView(libraryAlbumView)
            onContentYChanged: root.controller.scheduleLibraryViewStatePersist()

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
                    root.stepScrollView(libraryAlbumView, wheel, libraryAlbumView.rowHeight, 3)
                }
            }

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Menu
                        || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                    libraryAlbumView.openContextMenuForSelection()
                    event.accepted = true
                    return
                }
                root.controller.handleKeyPress(event)
            }

            // Opens the context menu from the keyboard, for the selected row.
            function openContextMenuForSelection() {
                const index = root.controller.currentSelectionIndex()
                if (index < 0) {
                    return
                }
                libraryAlbumView.positionViewAtIndex(index, ListView.Contain)
                const row = libraryAlbumView.itemAtIndex(index)
                if (row) {
                    row.openContextMenu(0, row.height)
                }
            }

            delegate: Rectangle {
                id: libraryRow
                objectName: "libraryRow"

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
                // The model's role, so a row costs the same with any
                // number of rows selected.
                readonly property bool rowSelected: !!model.selected
                readonly property string rowKindText: isTrackRow
                    ? "Track"
                    : (isAlbumRow
                        ? "Album"
                        : (rowTypeResolved === "artist"
                            ? "Artist"
                            : (rowTypeResolved === "root" ? "Library folder" : "")))

                function actionRowMap() {
                    return {
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
                }

                function openContextMenu(x, y) {
                    libraryContextMenu.rowMap = libraryRow.actionRowMap()
                    libraryContextMenu.popup(libraryRow, x, y)
                }

                width: ListView.view.width
                height: libraryAlbumView.rowHeight
                color: rowSelected
                    ? root.uiPalette.uiSelectionColor
                    : (index % 2 === 0
                        ? root.uiPalette.uiSurfaceRaisedColor
                        : root.uiPalette.uiSurfaceAltColor)

                Accessible.role: Accessible.TreeItem
                Accessible.name: rowTitle
                // QML cannot set the expanded state, so the description
                // tells it.
                Accessible.description: hasChildren
                    ? rowKindText + (expanded ? ", expanded" : ", collapsed")
                    : rowKindText
                Accessible.selectable: true
                Accessible.selected: rowSelected

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 3

                    Item {
                        Layout.preferredWidth: Math.max(
                            0, depthResolved * Math.round(libraryAlbumView.rowHeight * 0.75))
                    }

                    Label {
                        id: expanderIcon
                        Layout.preferredWidth: libraryAlbumView.rowHeight
                        Layout.fillHeight: true
                        Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: hasChildren ? (expanded ? "▾" : "▸") : ""
                        // The triangles are small in most fonts.
                        font.pointSize: root.uiPalette.basePointSize * 1.5
                        font.bold: true
                        color: rowSelected
                            ? root.uiPalette.uiSelectionTextColor
                            : root.uiPalette.uiMutedTextColor
                    }

                    Item {
                        visible: isAlbumRow || (isSectionRow && (coverPath || "") !== "")
                        Layout.preferredWidth: libraryAlbumView.rowHeight - 6
                        Layout.preferredHeight: libraryAlbumView.rowHeight - 6
                        Layout.alignment: Qt.AlignVCenter

                        Image {
                            anchors.fill: parent
                            source: albumCoverInViewport
                                ? root.uiBridge.libraryThumbnailSource(coverPath || "", coverStamp || "")
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
                        color: rowSelected
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
                        const rowMap = libraryRow.actionRowMap()
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
                            libraryRow.openContextMenu(mouse.x, mouse.y)
                        }
                    }

                    onDoubleClicked: function(mouse) {
                        const rowMap = libraryRow.actionRowMap()
                        if (hasChildren && mouse.x <= expanderIcon.x + expanderIcon.width + 6) {
                            root.controller.toggleNode(key)
                            return
                        }
                        root.controller.playRowItems(rowMap)
                    }
                }

                Menu {
                    id: libraryContextMenu
                    objectName: "libraryContextMenu"
                    property var rowMap: ({})
                    enter: Components.PopupTransition { duration: root.popupTransitionMs }
                    exit: Components.PopupTransition { duration: root.popupTransitionMs }

                    MenuItem {
                        text: "Play"
                        enabled: root.controller.isActionableRow(libraryContextMenu.rowMap)
                        onTriggered: root.controller.playRowItems(libraryContextMenu.rowMap)
                    }

                    MenuItem {
                        text: "Queue"
                        enabled: root.controller.isActionableRow(libraryContextMenu.rowMap)
                        onTriggered: root.controller.appendRowItems(libraryContextMenu.rowMap)
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

        ColumnLayout {
            id: libraryEmptyState
            readonly property bool loading: root.controller.isTreeLoading()
            visible: libraryAlbumView.count === 0
                && libraryAlbumView.contentHeight <= libraryAlbumView.height
            Layout.fillWidth: true
            spacing: 6

            Label {
                objectName: "libraryEmptyStateLabel"
                text: libraryEmptyState.loading
                    ? "Loading library..."
                    : (root.uiBridge.libraryRootCount > 0
                        ? "No music found in the library folders"
                        : "Add a folder with your music to the library")
                color: root.uiPalette.uiMutedTextColor
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
            }

            Button {
                objectName: "libraryAddFolderButton"
                visible: !libraryEmptyState.loading
                action: root.addLibraryFolderAction
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }
}
