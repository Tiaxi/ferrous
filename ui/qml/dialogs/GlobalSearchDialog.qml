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
    standardButtons: Dialog.NoButton
    padding: 10
    width: Math.min(1000, root.windowRoot.width - 64)
    height: Math.min(720, root.windowRoot.height - 80)
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }
    Component.onCompleted: root.controller.registerRefs(root, queryField, resultsView)
    onOpened: root.controller.handleDialogOpened(queryField.text || "")
    onClosed: root.controller.endOpen(true)

    header: Item {
        implicitHeight: 32
        Label {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            color: root.uiPalette.uiTextColor
        }
        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 28; height: 28
            text: "×"
            Accessible.name: "Close search"
            onClicked: root.close()
        }
    }
    footer: Item { implicitHeight: 0 }

    function escaped(value) {
        return String(value || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    function highlighted(value) {
        return root.controller.globalSearchModelApi.highlightText
            ? root.controller.globalSearchModelApi.highlightText(String(value || ""), queryField.text)
            : root.escaped(value)
    }
    function queryMatches(value) {
        return !!value && root.highlighted(value) !== root.escaped(value)
    }
    function rowContext(row, full) {
        let parts = []
        if (row.rowType === "track") parts.push(row.artist || "", row.album || "")
        else if (row.rowType === "album") parts.push(row.artist || "")
        if (row.year !== undefined && row.year !== null && row.year > 0) parts.push(String(row.year))
        if (row.genre && (full || root.queryMatches(row.genre))) parts.push(row.genre)
        if (row.rootLabel && (full || row.showRoot || root.queryMatches(row.rootLabel))) parts.push(row.rootLabel)
        if (row.matchDetail) parts.push(row.matchDetail)
        return parts.filter(function(value) { return value.length > 0 }).join(" · ")
    }
    function resultSummary() {
        const b = root.controller.uiBridge
        const shown = [b.globalSearchArtistCount || 0, b.globalSearchAlbumCount || 0, b.globalSearchTrackCount || 0]
        const totals = b.globalSearchTotals || shown
        const types = ["artist", "album", "track"]
        let parts = []
        for (let i = 0; i < 3; ++i) {
            if ((root.resultFilter === "all" && totals[i] > 0) || root.resultFilter === types[i]) {
                const count = shown[i] === totals[i] ? String(shown[i]) : shown[i] + " of " + totals[i]
                parts.push(count + " " + types[i] + (totals[i] === 1 ? "" : "s"))
            }
        }
        return parts.length ? parts.join(" · ") : "0 results"
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
            const next = root.controller.nextSelectableIndex(root.controller.selectedDisplayIndex, event.key === Qt.Key_Down ? 1 : -1, true)
            if (next >= 0) root.controller.selectDisplayIndex(next)
        } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
            root.controller.moveSelectionByPage(event.key === Qt.Key_PageDown ? 1 : -1)
        } else return false
        event.accepted = true
        return true
    }
    Connections {
        target: root.controller.globalSearchModelApi
        ignoreUnknownSignals: true
        function onSearchRowsChanged() {
            root.controller.selectFirstItem()
            resultsView.cancelFlick()
            resultsView.positionViewAtBeginning()
        }
    }

    contentItem: ColumnLayout {
        spacing: 6
        TextField {
            id: queryField
            objectName: "globalSearchQueryField"
            Layout.fillWidth: true
            placeholderText: "Search music, tags, or filenames"
            onTextChanged: {
                root.controller.selectedDisplayIndex = -1
                root.controller.uiBridge.setGlobalSearchQuery(text)
                if (!root.searching) root.controller.selectFirstItem()
            }
            Keys.onPressed: function(event) { root.handleResultKeys(event) }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: [{title: "All", type: "all"}, {title: "Albums", type: "album"}, {title: "Tracks", type: "track"}, {title: "Artists", type: "artist"}]
                Button {
                    text: modelData.title
                    implicitWidth: 64
                    implicitHeight: 28
                    checkable: true
                    checked: root.resultFilter === modelData.type
                    onClicked: root.controller.globalSearchModelApi.resultFilter = modelData.type
                }
            }
            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                horizontalAlignment: Text.AlignRight
                text: root.searching ? "Searching…" : root.resultSummary()
                color: root.uiPalette.uiMutedTextColor
                elide: Text.ElideRight
            }
            Button {
                objectName: "globalSearchShowMoreButton"
                text: "Show more"
                implicitHeight: 28
                visible: !!root.controller.uiBridge.globalSearchCanExpand && root.filterHasMore()
                onClicked: root.controller.uiBridge.expandGlobalSearch()
            }
            BusyIndicator { running: root.searching; visible: running; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
            ToolButton {
                objectName: "globalSearchHelpButton"
                text: "?"
                implicitWidth: 28; implicitHeight: 28
                Accessible.name: "Search syntax and keyboard help"
                onClicked: searchHelp.open()
                ToolTip.visible: hovered
                ToolTip.text: "Search syntax and keyboard help"
            }
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
                readonly property real resultRowHeight: Math.max(34, rowFont.height + 10)
                currentIndex: root.controller.selectedDisplayIndex
                highlightFollowsCurrentItem: false
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                opacity: root.controller.uiBridge.globalSearchModelRetained ? 0 : (root.searching ? 0.5 : 1)
                enabled: !root.searching && !root.controller.uiBridge.globalSearchModelRetained
                FontMetrics { id: rowFont; font: queryField.font }
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
                    readonly property bool selected: index === root.controller.selectedDisplayIndex
                    readonly property color foreground: selected ? root.uiPalette.uiSelectionTextColor : root.uiPalette.uiTextColor
                    readonly property string context: root.rowContext(model, false)
                    width: resultsView.width - (resultScrollBar.visible ? resultScrollBar.width : 0)
                    height: resultsView.resultRowHeight
                    color: selected ? root.uiPalette.uiSelectionColor
                        : (index % 2 ? root.uiPalette.uiSurfaceRaisedColor : root.uiPalette.uiSurfaceAltColor)
                    Accessible.role: Accessible.ListItem
                    Accessible.name: model.rowType + ": " + model.label + ", " + root.rowContext(model, true)
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6; anchors.rightMargin: 6
                        anchors.topMargin: 3; anchors.bottomMargin: 3
                        spacing: 8
                        RowLayout {
                            Layout.minimumWidth: Math.max(72, rowFont.advanceWidth("ARTIST") + 24)
                            Layout.maximumWidth: Layout.minimumWidth
                            Layout.fillWidth: false
                            spacing: 4
                            Components.SearchResultIcon {
                                Layout.preferredWidth: 16; Layout.preferredHeight: 16
                                resultType: model.rowType || "track"
                                foreground: resultRow.foreground
                            }
                            Label {
                                objectName: "globalSearchResultType"
                                Layout.fillWidth: true
                                text: model.rowType === "album" ? "Album" : (model.rowType === "artist" ? "Artist" : "Track")
                                font.weight: Font.Normal
                                color: resultRow.selected ? resultRow.foreground : root.uiPalette.uiMutedTextColor
                            }
                        }
                        Image {
                            Layout.minimumWidth: 24; Layout.maximumWidth: 24
                            Layout.preferredHeight: 24
                            source: model.coverUrl || ""
                            sourceSize.width: 48; sourceSize.height: 48
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Label {
                            objectName: "globalSearchResultName"
                            Layout.minimumWidth: resultRow.width * 0.30
                            Layout.maximumWidth: Layout.minimumWidth
                            text: root.highlighted(model.label)
                            textFormat: Text.StyledText
                            elide: Text.ElideRight
                            color: resultRow.foreground
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: root.highlighted(resultRow.context)
                            textFormat: Text.StyledText
                            elide: Text.ElideRight
                            color: resultRow.selected ? resultRow.foreground : root.uiPalette.uiMutedTextColor
                        }
                        Label {
                            Layout.minimumWidth: Math.max(74, rowFont.advanceWidth("999 tracks"))
                            Layout.maximumWidth: Layout.minimumWidth
                            horizontalAlignment: Text.AlignRight
                            text: model.rowType === "album" ? (model.count || 0) + " tracks" : (model.lengthText || "")
                            color: resultRow.foreground
                        }
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
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
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 700
                        ToolTip.text: model.label + "\n" + root.rowContext(model, true)
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: !root.searching && (root.controller.uiBridge.globalSearchModelRetained || resultsView.count === 0)
                text: queryField.text.trim().length === 0 ? "Type to search your library" : "No matches. Try fewer words or another result type."
                color: root.uiPalette.uiMutedTextColor
            }
        }
        Label {
            Layout.fillWidth: true
            visible: !root.searching && root.filterHasMore() && !root.controller.uiBridge.globalSearchCanExpand
            text: "More matches exist. Refine your search to narrow the results."
            color: root.uiPalette.uiMutedTextColor
            wrapMode: Text.Wrap
        }
        Popup {
            id: searchHelp
            objectName: "globalSearchHelp"
            focus: true
            width: Math.min(480, root.availableWidth)
            x: root.availableWidth - width
            y: queryField.height + 34
            padding: 12
            contentItem: Label {
                text: 'Use "quotes" for phrases. Combine words and filters to narrow results.\n\n'
                    + 'type:album · type:track · type:artist\n'
                    + 'title: · artist: · album: · albumartist: · genre: · year: · date:\n'
                    + 'composer: · conductor: · performer: · label: · comment: · lyrics:\n'
                    + 'root: · path: · track: · disc:\n\n'
                    + 'Example: signify year:1996 type:album\n\n'
                    + '↑/↓: select · Page Up/Down: move by page\n'
                    + 'Enter: play and replace queue\nCtrl+Enter: show in library · Ctrl+F: edit query · Esc: close'
                wrapMode: Text.Wrap
            }
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
