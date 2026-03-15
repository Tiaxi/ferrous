import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
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
            onTextChanged: root.controller.uiBridge.setGlobalSearchQuery(text)
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
                clip: true
                model: root.controller.uiBridge.globalSearchModel || []
                reuseItems: true
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                boundsMovement: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                pixelAligned: true
                readonly property int reservedRightPadding: globalSearchResultsScrollBar.visible
                    ? globalSearchResultsScrollBar.width + 8
                    : 8

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

                delegate: Rectangle {
                    readonly property string rowKind: typeof kind !== "undefined" ? (kind || "") : ""
                    readonly property string rowTypeValue: typeof rowType !== "undefined"
                        ? (rowType || "")
                        : ""
                    readonly property string sectionTitleValue: typeof sectionTitle !== "undefined"
                        ? (sectionTitle || "")
                        : ""
                    readonly property string labelValue: typeof label !== "undefined" ? (label || "") : ""
                    readonly property string artistValue: typeof artist !== "undefined"
                        ? (artist || "")
                        : ""
                    readonly property string albumValue: typeof album !== "undefined" ? (album || "") : ""
                    readonly property string rootLabelValue: typeof rootLabel !== "undefined"
                        ? (rootLabel || "")
                        : ""
                    readonly property string genreValue: typeof genre !== "undefined" ? (genre || "") : ""
                    readonly property string coverUrlValue: typeof coverUrl !== "undefined"
                        ? (coverUrl || "")
                        : ""
                    readonly property string lengthTextValue: typeof lengthText !== "undefined"
                        ? (lengthText || "")
                        : ""
                    readonly property var yearValue: typeof year !== "undefined" ? year : null
                    readonly property var trackNumberValue: typeof trackNumber !== "undefined"
                        ? trackNumber
                        : null
                    readonly property var countValue: typeof count !== "undefined" ? count : null
                    readonly property color rowTextColor: index === root.controller.selectedDisplayIndex
                        ? root.uiPalette.uiSelectionTextColor
                        : root.uiPalette.uiTextColor

                    width: Math.max(0, ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                    height: rowKind === "section" ? 30 : 24
                    color: rowKind === "section"
                        ? root.uiPalette.uiSectionColor
                        : (rowKind === "columns"
                            ? root.uiPalette.uiColumnsColor
                            : (index === root.controller.selectedDisplayIndex
                                ? root.uiPalette.uiSelectionColor
                                : (index % 2 === 0
                                    ? root.uiPalette.uiSurfaceRaisedColor
                                    : root.uiPalette.uiSurfaceAltColor)))
                    border.width: rowKind === "item" ? 0 : 1
                    border.color: rowKind === "section"
                        ? Qt.darker(root.uiPalette.uiSectionColor, 1.12)
                        : (rowKind === "columns"
                            ? Qt.darker(root.uiPalette.uiColumnsColor, 1.1)
                            : "transparent")

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        Label {
                            visible: rowKind === "section"
                            Layout.fillWidth: true
                            text: sectionTitleValue || ""
                            font.weight: Font.DemiBold
                            color: root.uiPalette.uiTextColor
                        }

                        RowLayout {
                            visible: rowKind === "columns" && rowTypeValue === "artist"
                            Layout.fillWidth: true
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

                        RowLayout {
                            visible: rowKind === "columns" && rowTypeValue === "album"
                            Layout.fillWidth: true
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

                        RowLayout {
                            visible: rowKind === "columns" && rowTypeValue === "track"
                            Layout.fillWidth: true
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

                        Loader {
                            visible: rowKind === "item"
                            Layout.fillWidth: true
                            sourceComponent: rowTypeValue === "artist"
                                ? globalSearchArtistItemComponent
                                : (rowTypeValue === "album"
                                    ? globalSearchAlbumItemComponent
                                    : globalSearchTrackItemComponent)
                        }
                    }

                    Component {
                        id: globalSearchArtistItemComponent

                        RowLayout {
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                text: labelValue || ""
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                visible: root.globalSearchShowsRootColumn
                                text: rootLabelValue || ""
                                Layout.preferredWidth: root.globalSearchRootColumnWidth
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                        }
                    }

                    Component {
                        id: globalSearchAlbumItemComponent

                        RowLayout {
                            spacing: 8

                            Item {
                                Layout.preferredWidth: root.globalSearchCoverColumnWidth
                                Layout.preferredHeight: 20

                                Image {
                                    anchors.fill: parent
                                    source: coverUrlValue || ""
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                    cache: true
                                    sourceSize.width: 32
                                    sourceSize.height: 32
                                }
                            }
                            Label {
                                text: labelValue || ""
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                text: artistValue || ""
                                Layout.preferredWidth: root.globalSearchArtistColumnWidth
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                visible: root.globalSearchShowsRootColumn
                                text: rootLabelValue || ""
                                Layout.preferredWidth: root.globalSearchRootColumnWidth
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                text: yearValue !== undefined && yearValue !== null ? yearValue : ""
                                Layout.preferredWidth: root.globalSearchYearColumnWidth
                                horizontalAlignment: Text.AlignRight
                                color: rowTextColor
                            }
                            Label {
                                text: genreValue || ""
                                Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                text: countValue !== undefined ? countValue : ""
                                Layout.preferredWidth: root.globalSearchAlbumCountColumnWidth
                                horizontalAlignment: Text.AlignRight
                                color: rowTextColor
                            }
                            Label {
                                text: lengthTextValue || "--:--"
                                Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                horizontalAlignment: Text.AlignRight
                                color: rowTextColor
                            }
                        }
                    }

                    Component {
                        id: globalSearchTrackItemComponent

                        RowLayout {
                            spacing: 8

                            Label {
                                text: trackNumberValue !== undefined && trackNumberValue !== null
                                    ? String(trackNumberValue).padStart(2, "0")
                                    : ""
                                Layout.preferredWidth: root.globalSearchTrackNumberColumnWidth
                                horizontalAlignment: Text.AlignRight
                                color: rowTextColor
                            }
                            Label {
                                text: labelValue || ""
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                text: artistValue || ""
                                Layout.preferredWidth: root.globalSearchArtistColumnWidth
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Item {
                                Layout.preferredWidth: root.globalSearchCoverColumnWidth
                                Layout.preferredHeight: 18

                                Image {
                                    anchors.fill: parent
                                    source: coverUrlValue || ""
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                    cache: true
                                    sourceSize.width: 24
                                    sourceSize.height: 24
                                }
                            }
                            Label {
                                text: albumValue || ""
                                Layout.preferredWidth: root.globalSearchAlbumColumnWidth
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                visible: root.globalSearchShowsRootColumn
                                text: rootLabelValue || ""
                                Layout.preferredWidth: root.globalSearchRootColumnWidth
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                text: yearValue !== undefined && yearValue !== null ? yearValue : ""
                                Layout.preferredWidth: root.globalSearchYearColumnWidth
                                horizontalAlignment: Text.AlignRight
                                color: rowTextColor
                            }
                            Label {
                                text: genreValue || ""
                                Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                elide: Text.ElideRight
                                color: rowTextColor
                            }
                            Label {
                                text: lengthTextValue || "--:--"
                                Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                horizontalAlignment: Text.AlignRight
                                color: rowTextColor
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: rowKind === "item"
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
