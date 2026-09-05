// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../components" as Components

Dialog {
    id: root
    objectName: "globalSearchDialog"
    required property var controller
    required property var uiPalette
    required property var windowRoot
    required property int popupTransitionMs
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property bool globalSearchShowsRootColumn
    property var contextRowData: ({})
    readonly property bool searching: !!root.controller.uiBridge.globalSearchBusy
    readonly property string resultFilter: root.controller.globalSearchModelApi.resultFilter || "all"

    modal: true
    title: "Search library"
    standardButtons: Dialog.Close
    width: Math.min(1000, root.windowRoot.width - 64)
    height: Math.min(720, root.windowRoot.height - 80)
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }
    Component.onCompleted: root.controller.registerRefs(root, queryField, resultsView)
    onOpened: root.controller.handleDialogOpened(queryField.text || "")
    onClosed: root.controller.endOpen(true)

    function highlighted(value) {
        const text = String(value || "")
        return root.controller.globalSearchModelApi.highlightText
            ? root.controller.globalSearchModelApi.highlightText(text, queryField.text)
            : text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    function resultSummary() {
        const bridge = root.controller.uiBridge
        const shown = [bridge.globalSearchArtistCount || 0, bridge.globalSearchAlbumCount || 0, bridge.globalSearchTrackCount || 0]
        const totals = bridge.globalSearchTotals || shown
        const names = ["artists", "albums", "tracks"]
        const types = ["artist", "album", "track"]
        let parts = []
        for (let i = 0; i < 3; ++i) {
            if (root.resultFilter === "all" || root.resultFilter === types[i])
                parts.push(shown[i] + " of " + (totals[i] || 0) + " " + names[i])
        }
        return parts.join(" · ")
    }

    function filterHasMore() {
        const b = root.controller.uiBridge
        const shown = [b.globalSearchArtistCount || 0, b.globalSearchAlbumCount || 0, b.globalSearchTrackCount || 0]
        const totals = b.globalSearchTotals || shown
        const types = ["artist", "album", "track"]
        return types.some(function(type, i) { return (root.resultFilter === "all" || root.resultFilter === type) && shown[i] < totals[i] })
    }

    function handleResultKeys(event) {
        if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_F) {
            root.controller.focusQueryField(true)
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (event.modifiers & Qt.ControlModifier) root.controller.navigateSelectionToLibrary()
            else root.controller.activateSelection()
        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
            const direction = event.key === Qt.Key_Down ? 1 : -1
            const next = root.controller.nextSelectableIndex(root.controller.selectedDisplayIndex, direction, true)
            if (next >= 0) root.controller.selectDisplayIndex(next)
        } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
            root.controller.moveSelectionByPage(event.key === Qt.Key_PageDown ? 1 : -1)
        } else {
            return false
        }
        event.accepted = true
        return true
    }

    Connections {
        target: root.controller.globalSearchModelApi
        ignoreUnknownSignals: true
        function onSearchRowsChanged() {
            root.controller.selectFirstItem()
            resultsView.positionViewAtBeginning()
        }
    }

    contentItem: ColumnLayout {
        spacing: 8
        TextField {
            id: queryField
            objectName: "globalSearchQueryField"
            Layout.fillWidth: true
            placeholderText: "Search title, artist, album, genre, or filename"
            onTextChanged: {
                root.controller.selectedDisplayIndex = -1
                root.controller.uiBridge.setGlobalSearchQuery(text)
                if (!root.searching) root.controller.selectFirstItem()
            }
            Keys.onPressed: function(event) { root.handleResultKeys(event) }
        }
        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: [{title: "All", type: "all"}, {title: "Artists", type: "artist"}, {title: "Albums", type: "album"}, {title: "Tracks", type: "track"}]
                Button {
                    text: modelData.title
                    checkable: true
                    checked: root.resultFilter === modelData.type
                    onClicked: root.controller.globalSearchModelApi.resultFilter = modelData.type
                }
            }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: root.searching; visible: running; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
        }
        Label {
            Layout.fillWidth: true
            text: root.searching ? "Searching…" : root.resultSummary()
            color: root.uiPalette.uiMutedTextColor
            wrapMode: Text.Wrap
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "transparent"
            border.color: root.uiPalette.uiBorderColor
            ListView {
                id: resultsView
                objectName: "globalSearchResultsView"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: root.controller.uiBridge.globalSearchModel || []
                reuseItems: true
                boundsBehavior: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                opacity: root.controller.uiBridge.globalSearchModelRetained ? 0 : (root.searching ? 0.5 : 1)
                enabled: !root.searching && !root.controller.uiBridge.globalSearchModelRetained
                ScrollBar.vertical: ScrollBar { id: resultScrollBar }
                Keys.onPressed: function(event) {
                    if (root.handleResultKeys(event)) return
                    if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                        root.controller.selectDisplayIndex(event.key === Qt.Key_Home
                            ? root.controller.searchFirstSelectableIndex() : root.controller.searchLastSelectableIndex())
                        event.accepted = true
                    }
                }
                delegate: Rectangle {
                    id: resultRow
                    readonly property bool section: model.kind === "section"
                    readonly property bool selected: index === root.controller.selectedDisplayIndex
                    readonly property color foreground: selected ? root.uiPalette.uiSelectionTextColor : root.uiPalette.uiTextColor
                    readonly property string detail: {
                        let parts = []
                        if (model.rowType === "track") parts.push(model.artist || "", model.album || "")
                        else if (model.rowType === "album") parts.push(model.artist || "")
                        if (model.year !== undefined && model.year !== null) parts.push(String(model.year))
                        if (model.genre) parts.push(model.genre)
                        if (root.globalSearchShowsRootColumn && model.rootLabel) parts.push(model.rootLabel)
                        if (model.score >= 6 && model.trackPath) parts.push("Matched path: " + model.trackPath)
                        return parts.filter(function(value) { return value.length > 0 }).join(" · ")
                    }
                    width: resultsView.width - (resultScrollBar.visible ? resultScrollBar.width : 0)
                    height: section ? 30 : 60
                    color: section ? root.uiPalette.uiSectionColor : selected ? root.uiPalette.uiSelectionColor
                        : (index % 2 ? root.uiPalette.uiSurfaceRaisedColor : root.uiPalette.uiSurfaceAltColor)
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 10
                        Image {
                            visible: !resultRow.section && model.rowType !== "artist"
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: 40
                            source: model.coverUrl || ""
                            sourceSize.width: 80
                            sourceSize.height: 80
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: resultRow.section ? model.sectionTitle : root.highlighted(model.label)
                                textFormat: Text.StyledText
                                font.weight: resultRow.section ? Font.DemiBold : Font.Normal
                                elide: Text.ElideRight
                                color: resultRow.foreground
                            }
                            Label {
                                visible: !resultRow.section && resultRow.detail.length > 0
                                Layout.fillWidth: true
                                text: root.highlighted(resultRow.detail)
                                textFormat: Text.StyledText
                                elide: Text.ElideRight
                                color: resultRow.selected ? resultRow.foreground : root.uiPalette.uiMutedTextColor
                            }
                        }
                        Label {
                            visible: !resultRow.section
                            text: model.rowType === "track" ? "Track" : model.rowType === "album" ? "Album" : "Artist"
                            color: resultRow.foreground
                        }
                        Label {
                            visible: !resultRow.section && model.rowType !== "artist"
                            text: (model.rowType === "album" ? (model.count || 0) + " tracks · " : "") + (model.lengthText || "")
                            color: resultRow.foreground
                        }
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        enabled: !resultRow.section
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: function(mouse) {
                            root.controller.selectDisplayIndex(index)
                            if (mouse.button === Qt.RightButton) {
                                root.contextRowData = root.controller.globalSearchModelApi.rowDataAt(index)
                                resultMenu.popup()
                            } else resultsView.forceActiveFocus()
                        }
                        onDoubleClicked: function(mouse) {
                            if (mouse.button === Qt.LeftButton) root.controller.activateSelection()
                        }
                        ToolTip.visible: containsMouse && !resultRow.section
                        ToolTip.delay: 700
                        ToolTip.text: (model.label || "") + "\n" + resultRow.detail
                    }
                }

            }
                Label {
                    anchors.centerIn: parent
                    visible: !root.searching && ((root.controller.uiBridge.globalSearchModelRetained || resultsView.count === 0))
                    text: queryField.text.trim().length === 0 ? "Type to search your library" : "No matches. Try fewer words or another result type."
                    color: root.uiPalette.uiMutedTextColor
                }
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: "Enter: play and replace queue · Ctrl+Enter: show in library · Esc: close"
                color: root.uiPalette.uiMutedTextColor
                wrapMode: Text.Wrap
            }
            Button {
                text: "Show more"
                visible: !!root.controller.uiBridge.globalSearchCanExpand && root.filterHasMore()
                onClicked: root.controller.uiBridge.expandGlobalSearch()
            }
        }
        Label {
            Layout.fillWidth: true
            visible: !root.searching && root.filterHasMore() && !root.controller.uiBridge.globalSearchCanExpand
            text: "More matches exist. Refine your search to narrow the results."
            color: root.uiPalette.uiMutedTextColor
            wrapMode: Text.Wrap
        }
        Menu {
            id: resultMenu
            MenuItem { text: "Play and replace queue"; onTriggered: root.controller.activateRow(root.contextRowData) }
            MenuItem { text: "Queue"; onTriggered: root.controller.queueRow(root.contextRowData) }
            MenuItem { text: "Show in library"; onTriggered: root.controller.navigateSelectionToLibrary() }
            MenuSeparator {}
            MenuItem { text: "Open containing folder"; onTriggered: root.controller.openRowInFileBrowser(root.contextRowData) }
        }
    }
}
