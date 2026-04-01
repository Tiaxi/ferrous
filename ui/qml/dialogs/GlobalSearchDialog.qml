// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQml.Models
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

Dialog {
    id: root

    required property var controller
    required property var uiPalette
    required property var windowRoot
    required property int popupTransitionMs
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property bool globalSearchShowsRootColumn
    required property int globalSearchTrackNumberColumnWidth
    required property int globalSearchCoverColumnWidth
    required property int globalSearchArtistColumnWidth
    required property int globalSearchAlbumColumnWidth
    required property int globalSearchRootColumnWidth
    required property int globalSearchYearColumnWidth
    required property int globalSearchTrackGenreColumnWidth
    required property int globalSearchAlbumCountColumnWidth
    required property int globalSearchTrackLengthColumnWidth

    property var contextRowData: ({})

    modal: true
    title: "Global Search"
    standardButtons: Dialog.Close
    width: Math.min(1240, root.windowRoot.width - 64)
    height: Math.min(720, root.windowRoot.height - 80)
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }

    Component.onCompleted: root.controller.registerRefs(root, globalSearchQueryField, globalSearchResultsView)

    onOpened: root.controller.handleDialogOpened(globalSearchQueryField.text || "")
    onClosed: root.controller.endOpen(true)

    contentItem: ColumnLayout {
        spacing: 8

        TextField {
            id: globalSearchQueryField
            Layout.fillWidth: true
            placeholderText: "Type artist, album, or track"
            onTextChanged: {
                root.controller.uiBridge.setGlobalSearchQuery(text)
                root.controller.selectFirstItem()
                globalSearchResultsView.positionViewAtBeginning()
            }
            Keys.onPressed: function(event) {
                if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_F) {
                    root.controller.focusQueryField(!root.controller.ignoreRefocusFind)
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                    root.controller.navigateSelectionToLibrary()
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_Down) {
                    const next = root.controller.nextSelectableIndex(
                        root.controller.selectedDisplayIndex,
                        1,
                        true)
                    if (next >= 0) {
                        root.controller.selectDisplayIndex(next)
                        globalSearchResultsView.forceActiveFocus()
                    }
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_Up) {
                    const next = root.controller.nextSelectableIndex(
                        root.controller.selectedDisplayIndex,
                        -1,
                        true)
                    if (next >= 0) {
                        root.controller.selectDisplayIndex(next)
                        globalSearchResultsView.forceActiveFocus()
                    }
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_PageDown) {
                    if (root.controller.moveSelectionByPage(1)) {
                        globalSearchResultsView.forceActiveFocus()
                    }
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_PageUp) {
                    if (root.controller.moveSelectionByPage(-1)) {
                        globalSearchResultsView.forceActiveFocus()
                    }
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_Home) {
                    const first = root.controller.searchFirstSelectableIndex()
                    if (first >= 0) {
                        root.controller.selectDisplayIndex(first)
                        globalSearchResultsView.forceActiveFocus()
                    }
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_End) {
                    const last = root.controller.searchLastSelectableIndex()
                    if (last >= 0) {
                        root.controller.selectDisplayIndex(last)
                        globalSearchResultsView.forceActiveFocus()
                    }
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    root.controller.activateSelection()
                    event.accepted = true
                }
            }
        }

        Label {
            Layout.fillWidth: true
            color: root.uiPalette.uiMutedTextColor
            text: "Artists: " + (root.controller.uiBridge.globalSearchArtistCount || 0)
                + " | Albums: " + (root.controller.uiBridge.globalSearchAlbumCount || 0)
                + " | Tracks: " + (root.controller.uiBridge.globalSearchTrackCount || 0)
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.uiPalette.uiSurfaceRaisedColor
            border.color: root.uiPalette.uiBorderColor

            ListView {
                id: globalSearchResultsView
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: root.controller.uiBridge.globalSearchModel || []
                reuseItems: true
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                boundsMovement: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                pixelAligned: true
                opacity: root.controller.uiBridge.globalSearchModelRetained ? 0 : 1
                enabled: !root.controller.uiBridge.globalSearchModelRetained
                readonly property int reservedRightPadding: globalSearchResultsScrollBar.visible
                    ? globalSearchResultsScrollBar.width
                    : 0

                ScrollBar.vertical: ScrollBar {
                    id: globalSearchResultsScrollBar
                    policy: ScrollBar.AsNeeded
                }

                Keys.onPressed: function(event) {
                    if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_F) {
                        root.controller.focusQueryField(!root.controller.ignoreRefocusFind)
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                        root.controller.navigateSelectionToLibrary()
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Down) {
                        const next = root.controller.nextSelectableIndex(
                            root.controller.selectedDisplayIndex,
                            1,
                            true)
                        if (next >= 0) {
                            root.controller.selectDisplayIndex(next)
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Up) {
                        const next = root.controller.nextSelectableIndex(
                            root.controller.selectedDisplayIndex,
                            -1,
                            true)
                        if (next >= 0) {
                            root.controller.selectDisplayIndex(next)
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_PageDown) {
                        root.controller.moveSelectionByPage(1)
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_PageUp) {
                        root.controller.moveSelectionByPage(-1)
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Home) {
                        const first = root.controller.searchFirstSelectableIndex()
                        if (first >= 0) {
                            root.controller.selectDisplayIndex(first)
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_End) {
                        const last = root.controller.searchLastSelectableIndex()
                        if (last >= 0) {
                            root.controller.selectDisplayIndex(last)
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        root.controller.activateSelection()
                        event.accepted = true
                    }
                }

                delegate: DelegateChooser {
                    role: "delegateType"

                    DelegateChoice {
                        roleValue: "section"
                        delegate: Rectangle {
                            width: Math.max(0, ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                            height: 30
                            color: root.uiPalette.uiSectionColor

                            Rectangle { visible: index > 0; height: 1; anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right; color: Qt.darker(root.uiPalette.uiSectionColor, 1.12) }
                            Rectangle { height: 1; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; color: Qt.darker(root.uiPalette.uiSectionColor, 1.12) }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    text: model.sectionTitle || ""
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiTextColor
                                }
                            }
                        }
                    }

                    DelegateChoice {
                        roleValue: "columns-artist"
                        delegate: Rectangle {
                            width: Math.max(0, ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                            height: 24
                            color: root.uiPalette.uiColumnsColor

                            Rectangle { height: 1; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; color: Qt.darker(root.uiPalette.uiColumnsColor, 1.1) }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                Label {
                                    text: "Name"
                                    Layout.fillWidth: true
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: "Root"
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                            }
                        }
                    }

                    DelegateChoice {
                        roleValue: "columns-album"
                        delegate: Rectangle {
                            width: Math.max(0, ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                            height: 24
                            color: root.uiPalette.uiColumnsColor

                            Rectangle { height: 1; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; color: Qt.darker(root.uiPalette.uiColumnsColor, 1.1) }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                Label {
                                    text: ""
                                    Layout.preferredWidth: root.globalSearchCoverColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: "Title"
                                    Layout.fillWidth: true
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: "Artist"
                                    Layout.preferredWidth: root.globalSearchArtistColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: "Root"
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: "Year"
                                    Layout.preferredWidth: root.globalSearchYearColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label {
                                    text: "Genre"
                                    Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: "#"
                                    Layout.preferredWidth: root.globalSearchAlbumCountColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label {
                                    text: "Length"
                                    Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                        }
                    }

                    DelegateChoice {
                        roleValue: "columns-track"
                        delegate: Rectangle {
                            width: Math.max(0, ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                            height: 24
                            color: root.uiPalette.uiColumnsColor

                            Rectangle { height: 1; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; color: Qt.darker(root.uiPalette.uiColumnsColor, 1.1) }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                Label {
                                    text: "#"
                                    Layout.preferredWidth: root.globalSearchTrackNumberColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label {
                                    text: "Title"
                                    Layout.fillWidth: true
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: "Artist"
                                    Layout.preferredWidth: root.globalSearchArtistColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: ""
                                    Layout.preferredWidth: root.globalSearchCoverColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: "Album"
                                    Layout.preferredWidth: root.globalSearchAlbumColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: "Root"
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: "Year"
                                    Layout.preferredWidth: root.globalSearchYearColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label {
                                    text: "Genre"
                                    Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                }
                                Label {
                                    text: "Length"
                                    Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                        }
                    }

                    DelegateChoice {
                        roleValue: "artist"
                        delegate: Rectangle {
                            readonly property color rowTextColor: index === root.controller.selectedDisplayIndex
                                ? root.uiPalette.uiSelectionTextColor
                                : root.uiPalette.uiTextColor

                            width: Math.max(0, ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                            height: 24
                            color: index === root.controller.selectedDisplayIndex
                                ? root.uiPalette.uiSelectionColor
                                : (index % 2 === 0
                                    ? root.uiPalette.uiSurfaceRaisedColor
                                    : root.uiPalette.uiSurfaceAltColor)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    text: model.label || ""
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: model.rootLabel || ""
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: function(mouse) {
                                    root.controller.selectDisplayIndex(index)
                                    if (mouse.button === Qt.RightButton) {
                                        root.contextRowData = root.controller.globalSearchModelApi
                                            ? (root.controller.globalSearchModelApi.rowDataAt(index) || ({}))
                                            : ({})
                                        globalSearchContextMenu.popup()
                                        return
                                    }
                                    if (mouse.button === Qt.LeftButton) {
                                        globalSearchResultsView.forceActiveFocus()
                                    }
                                }
                                onDoubleClicked: function(mouse) {
                                    if (mouse.button === Qt.LeftButton) {
                                        root.controller.selectDisplayIndex(index)
                                        root.controller.activateSelection()
                                    }
                                }
                            }
                        }
                    }

                    DelegateChoice {
                        roleValue: "album"
                        delegate: Rectangle {
                            readonly property color rowTextColor: index === root.controller.selectedDisplayIndex
                                ? root.uiPalette.uiSelectionTextColor
                                : root.uiPalette.uiTextColor

                            width: Math.max(0, ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                            height: 24
                            color: index === root.controller.selectedDisplayIndex
                                ? root.uiPalette.uiSelectionColor
                                : (index % 2 === 0
                                    ? root.uiPalette.uiSurfaceRaisedColor
                                    : root.uiPalette.uiSurfaceAltColor)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                Item {
                                    Layout.preferredWidth: root.globalSearchCoverColumnWidth
                                    Layout.preferredHeight: 20

                                    Image {
                                        anchors.fill: parent
                                        source: model.coverUrl || ""
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        cache: true
                                        sourceSize.width: 32
                                        sourceSize.height: 32
                                    }
                                }
                                Label {
                                    text: model.label || ""
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.artist || ""
                                    Layout.preferredWidth: root.globalSearchArtistColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: model.rootLabel || ""
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.year !== undefined && model.year !== null ? model.year : ""
                                    Layout.preferredWidth: root.globalSearchYearColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.genre || ""
                                    Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.count !== undefined ? model.count : ""
                                    Layout.preferredWidth: root.globalSearchAlbumCountColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.lengthText || "--:--"
                                    Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: function(mouse) {
                                    root.controller.selectDisplayIndex(index)
                                    if (mouse.button === Qt.RightButton) {
                                        root.contextRowData = root.controller.globalSearchModelApi
                                            ? (root.controller.globalSearchModelApi.rowDataAt(index) || ({}))
                                            : ({})
                                        globalSearchContextMenu.popup()
                                        return
                                    }
                                    if (mouse.button === Qt.LeftButton) {
                                        globalSearchResultsView.forceActiveFocus()
                                    }
                                }
                                onDoubleClicked: function(mouse) {
                                    if (mouse.button === Qt.LeftButton) {
                                        root.controller.selectDisplayIndex(index)
                                        root.controller.activateSelection()
                                    }
                                }
                            }
                        }
                    }

                    DelegateChoice {
                        roleValue: "track"
                        delegate: Rectangle {
                            readonly property color rowTextColor: index === root.controller.selectedDisplayIndex
                                ? root.uiPalette.uiSelectionTextColor
                                : root.uiPalette.uiTextColor

                            width: Math.max(0, ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                            height: 24
                            color: index === root.controller.selectedDisplayIndex
                                ? root.uiPalette.uiSelectionColor
                                : (index % 2 === 0
                                    ? root.uiPalette.uiSurfaceRaisedColor
                                    : root.uiPalette.uiSurfaceAltColor)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                Label {
                                    text: model.trackNumber !== undefined && model.trackNumber !== null
                                        ? String(model.trackNumber).padStart(2, "0")
                                        : ""
                                    Layout.preferredWidth: root.globalSearchTrackNumberColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.label || ""
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.artist || ""
                                    Layout.preferredWidth: root.globalSearchArtistColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Item {
                                    Layout.preferredWidth: root.globalSearchCoverColumnWidth
                                    Layout.preferredHeight: 18

                                    Image {
                                        anchors.fill: parent
                                        source: model.coverUrl || ""
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        cache: true
                                        sourceSize.width: 24
                                        sourceSize.height: 24
                                    }
                                }
                                Label {
                                    text: model.album || ""
                                    Layout.preferredWidth: root.globalSearchAlbumColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: model.rootLabel || ""
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.year !== undefined && model.year !== null ? model.year : ""
                                    Layout.preferredWidth: root.globalSearchYearColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.genre || ""
                                    Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: model.lengthText || "--:--"
                                    Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: function(mouse) {
                                    root.controller.selectDisplayIndex(index)
                                    if (mouse.button === Qt.RightButton) {
                                        root.contextRowData = root.controller.globalSearchModelApi
                                            ? (root.controller.globalSearchModelApi.rowDataAt(index) || ({}))
                                            : ({})
                                        globalSearchContextMenu.popup()
                                        return
                                    }
                                    if (mouse.button === Qt.LeftButton) {
                                        globalSearchResultsView.forceActiveFocus()
                                    }
                                }
                                onDoubleClicked: function(mouse) {
                                    if (mouse.button === Qt.LeftButton) {
                                        root.controller.selectDisplayIndex(index)
                                        root.controller.activateSelection()
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Menu {
                id: globalSearchContextMenu
                property var rowData: root.contextRowData || ({})

                enter: Components.PopupTransition { duration: root.popupTransitionMs }
                exit: Components.PopupTransition { duration: root.popupTransitionMs }

                MenuItem {
                    text: "Play"
                    enabled: (globalSearchContextMenu.rowData.kind || "") === "item"
                    onTriggered: root.controller.activateRow(globalSearchContextMenu.rowData)
                }
                MenuItem {
                    text: "Queue"
                    enabled: (globalSearchContextMenu.rowData.kind || "") === "item"
                    onTriggered: root.controller.queueRow(globalSearchContextMenu.rowData)
                }
                MenuSeparator {}
                MenuItem {
                    text: "Open in " + root.controller.uiBridge.fileBrowserName
                    visible: (globalSearchContextMenu.rowData.rowType || "") !== "track"
                    enabled: (globalSearchContextMenu.rowData.kind || "") === "item"
                    onTriggered: root.controller.openRowInFileBrowser(globalSearchContextMenu.rowData)
                }
                MenuItem {
                    text: "Open containing folder"
                    visible: (globalSearchContextMenu.rowData.rowType || "") === "track"
                    enabled: (globalSearchContextMenu.rowData.kind || "") === "item"
                    onTriggered: root.controller.openRowInFileBrowser(globalSearchContextMenu.rowData)
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: (root.controller.uiBridge.globalSearchArtistCount || 0) === 0
                && (root.controller.uiBridge.globalSearchAlbumCount || 0) === 0
                && (root.controller.uiBridge.globalSearchTrackCount || 0) === 0
            text: (globalSearchQueryField.text || "").trim().length === 0
                ? "Type to search"
                : "No matches"
            color: Kirigami.Theme.disabledTextColor
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
