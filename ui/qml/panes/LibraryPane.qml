// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/ColorUtils.js" as ColorUtils

// The library: its region header, then the tree, on the view colour.
//
// The tree has one indent step: each level moves the chevron 16 px. An
// album row shows its thumbnail after the chevron and its year at the right
// end; a track row shows its playing or warning glyph in its album's chevron
// column, its number under the album's thumbnail and its title where the
// album's name starts.
Rectangle {
    id: root

    required property var controller
    required property var uiBridge
    required property var libraryModel
    required property var uiPalette
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property var stepScrollView
    required property var addLibraryFolderAction
    // The tracks the playback failed to read, from the issue history.
    property var errorController: null
    // A narrow sidebar: the header shows only the track total.
    property bool narrow: false

    // The tree's geometry: the inset of the first chevron, the indent step,
    // and the chevron, thumbnail and gaps after it.
    readonly property int rowInset: 12
    readonly property int indentStep: 16
    readonly property int chevronSize: 16
    readonly property int chevronGap: 6
    readonly property int thumbnailSize: 20
    readonly property int thumbnailGap: 8
    readonly property int stateGlyphSize: 12

    // The playing track, and what the playback is doing with it.
    readonly property string currentTrackPath: root.uiBridge.currentTrackPath || ""
    readonly property string playbackState: root.uiBridge.playbackState || ""
    // The keys of the rows that hold the playing track, as the backend
    // groups the tree: one artist row can gather tracks from more than one
    // folder, so the rows' paths cannot tell.
    readonly property var currentTrackTreeKeys: root.uiBridge.currentTrackTreeKeys || []

    // A track change: the rows that become now are amber at once, from
    // their own state; the track row that was now, or the collapsed rows
    // that held it, fade their amber out (#373 Motion). `nowTrack` is the
    // track that is amber, with the rows that hold it, as last seen. The
    // backend names the holding rows a moment after the track, so the
    // last names it gave are kept until the track changes.
    property var nowTrack: null
    readonly property QtObject nowFade: treeNowFade

    Components.NowFadeOut {
        id: treeNowFade
    }

    function followNowTrack() {
        const path = root.uiBridge.currentTrackPath || ""
        const state = root.uiBridge.playbackState || "Stopped"
        const keys = root.uiBridge.currentTrackTreeKeys || []
        const was = root.nowTrack
        if (was !== null && path !== was.path) {
            root.nowFade.start(was)
        }
        root.nowTrack = state !== "Stopped" && path.length > 0
            ? {
                path: path,
                keys: keys.length > 0 || was === null || was.path !== path ? keys : was.keys,
                glyph: state === "Paused" ? "media-playback-pause" : "media-playback-start"
            }
            : null
    }

    onCurrentTrackPathChanged: root.followNowTrack()
    onPlaybackStateChanged: root.followNowTrack()
    onCurrentTrackTreeKeysChanged: root.followNowTrack()
    Component.onCompleted: root.followNowTrack()

    readonly property string totalsText: {
        const tracks = root.uiBridge.libraryTrackCount
        const parts = []
        if (tracks > 0) {
            const trackText = I18n.i18ncp("@info:status library totals", "%1 track", "%1 tracks",
                                          tracks)
            if (root.narrow) {
                parts.push(trackText)
            } else {
                parts.push(I18n.i18ncp("@info:status library totals", "%1 artist", "%1 artists",
                                       root.uiBridge.libraryArtistCount))
                parts.push(I18n.i18ncp("@info:status library totals", "%1 album", "%1 albums",
                                       root.uiBridge.libraryAlbumCount))
                parts.push(trackText)
            }
        }
        // A scan's progress is the status bar's.
        return parts.join(" · ")
    }

    color: root.uiPalette.view

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Components.RegionHeader {
            id: libraryHeader
            objectName: "libraryHeader"
            Layout.fillWidth: true
            uiPalette: root.uiPalette
            leftInset: root.rowInset
            rightInset: root.rowInset
            title: I18n.i18nc("@title library region", "Library")
            totals: root.totalsText
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: libraryAlbumView
                objectName: "libraryView"
                // The controller pages and scrolls by it.
                readonly property real rowHeight: root.uiPalette.rowHeight
                anchors.fill: parent
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
                Accessible.name: I18n.i18nc("@label", "Library")

                // Expanding a row opens its children downward: they are in
                // place at once, under the rows below, which slide down and
                // uncover them; collapsing slides those rows back up over
                // the children as they fade. The toggled row and the rows
                // above it stay put. Only a toggle moves: the controller
                // sets `animatesToggle` while the view lays it out, so
                // scans, searches and restores change the rows at once.
                property bool animatesToggle: false
                readonly property int toggleDuration: Kirigami.Units.shortDuration
                readonly property bool togglesMove: libraryAlbumView.animatesToggle
                    && libraryAlbumView.toggleDuration > 0
                readonly property QtObject openAnimation: treeOpenAnimation
                readonly property QtObject closeAnimation: treeCloseAnimation
                readonly property QtObject closeFadeAnimation: treeCloseFadeAnimation
                addDisplaced: Transition {
                    objectName: "libraryOpenTransition"
                    enabled: libraryAlbumView.togglesMove
                    SequentialAnimation {
                        PropertyAction { property: "z"; value: 2 }
                        NumberAnimation {
                            id: treeOpenAnimation
                            objectName: "libraryOpenAnimation"
                            property: "y"
                            duration: libraryAlbumView.toggleDuration
                            easing.type: Easing.OutCubic
                        }
                        PropertyAction { property: "z"; value: 1 }
                    }
                }
                remove: Transition {
                    objectName: "libraryCloseRemoveTransition"
                    enabled: libraryAlbumView.togglesMove
                    SequentialAnimation {
                        NumberAnimation {
                            id: treeCloseFadeAnimation
                            objectName: "libraryCloseFadeAnimation"
                            property: "opacity"
                            to: 0
                            duration: libraryAlbumView.toggleDuration
                            easing.type: Easing.OutCubic
                        }
                        // Pooled for reuse, the row must come back whole.
                        PropertyAction { property: "opacity"; value: 1 }
                    }
                }
                removeDisplaced: Transition {
                    objectName: "libraryCloseTransition"
                    enabled: libraryAlbumView.togglesMove
                    SequentialAnimation {
                        PropertyAction { property: "z"; value: 2 }
                        NumberAnimation {
                            id: treeCloseAnimation
                            objectName: "libraryCloseAnimation"
                            property: "y"
                            duration: libraryAlbumView.toggleDuration
                            easing.type: Easing.OutCubic
                        }
                        PropertyAction { property: "z"; value: 1 }
                    }
                }

                Component.onCompleted: root.controller.registerView(libraryAlbumView)
                // The keyboard comes in: a row shows where it is.
                onActiveFocusChanged: {
                    if (libraryAlbumView.activeFocus) {
                        root.controller.ensureCurrentRow()
                    }
                }
                onContentYChanged: root.controller.scheduleLibraryViewStatePersist()

                onContentHeightChanged: {
                    if (root.controller.pendingExpandFitKey.length > 0) {
                        Qt.callLater(function() {
                            root.controller.applyPendingExpansionFit()
                        })
                    }
                }

                ScrollBar.vertical: Components.LaneScrollBar {
                    objectName: "libraryScrollBar"
                    uiPalette: root.uiPalette
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

                delegate: Item {
                    id: libraryRow
                    objectName: "libraryRow"

                    readonly property string rowTypeResolved: rowType || ""
                    readonly property bool isAlbumRow: rowTypeResolved === "album"
                    readonly property bool isSectionRow: rowTypeResolved === "section"
                    readonly property bool isTrackRow: rowTypeResolved === "track"
                    readonly property bool hasChildren: !isTrackRow && (count || 0) > 0
                    readonly property bool isExpanded: !!expanded
                    readonly property string selectionKeyResolved: selectionKey || ""
                    readonly property string artistResolved: artist || ""
                    readonly property string nameResolved: name || ""
                    readonly property string trackPathResolved: trackPath || ""
                    readonly property string openPathResolved: openPath || ""
                    readonly property var playPathsResolved: playPaths || []
                    readonly property int yearResolved: year || 0
                    readonly property int trackNumberResolved: trackNumber || 0
                    readonly property bool draggableLibraryItem: isTrackRow
                        || rowTypeResolved === "album"
                        || rowTypeResolved === "artist"
                        || playPathsResolved.length > 0
                    // An artist row shows the artist's name; its title also
                    // counts the albums.
                    readonly property string rowTitle: rowTypeResolved === "artist" && artistResolved.length > 0
                        ? artistResolved
                        : (title || name || artist || "")
                    readonly property bool showsThumbnail: isAlbumRow
                        || (isSectionRow && (coverPath || "") !== "")
                    readonly property bool albumCoverInViewport: showsThumbnail
                        && (y + height >= libraryAlbumView.contentY - 48)
                        && (y <= libraryAlbumView.contentY + libraryAlbumView.height + 48)
                    readonly property int sourceIndexResolved: sourceIndex !== undefined ? sourceIndex : -1
                    readonly property int depthResolved: depth !== undefined ? depth : 0
                    // The model's role, so a row costs the same with any
                    // number of rows selected.
                    readonly property bool rowSelected: !!model.selected
                    // The row the keyboard is on, in a list with focus.
                    readonly property bool rowCurrent: libraryAlbumView.activeFocus
                        && selectionKeyResolved.length > 0
                        && selectionKeyResolved === root.controller.currentRowKey
                    readonly property string rowKindText: isTrackRow
                        ? I18n.i18nc("@info library row kind", "Track")
                        : (isAlbumRow
                            ? I18n.i18nc("@info library row kind", "Album")
                            : (rowTypeResolved === "artist"
                                ? I18n.i18nc("@info library row kind", "Artist")
                                : (rowTypeResolved === "root"
                                    ? I18n.i18nc("@info library row kind", "Library folder")
                                    : "")))

                    // The playing track's row: what the playback does with it.
                    readonly property bool isCurrentTrack: isTrackRow
                        && root.currentTrackPath.length > 0
                        && trackPathResolved === root.currentTrackPath
                    readonly property bool isNow: isCurrentTrack && root.playbackState !== "Stopped"
                    readonly property string unreadableReason: isTrackRow && root.errorController
                        ? (root.errorController.unreadablePaths[trackPathResolved] || "")
                        : ""
                    // A collapsed row that holds the playing track.
                    readonly property bool holdsNow: hasChildren && !isExpanded
                        && root.playbackState !== "Stopped"
                        && root.currentTrackTreeKeys.indexOf(key || "") >= 0
                    // The amber left on the track row that was now, or on
                    // a collapsed row that held it, fading out after a
                    // track change; 0 on every other row.
                    readonly property real amberLeft: {
                        const fading = treeNowFade.target
                        if (fading === null) {
                            return 0
                        }
                        if (isTrackRow) {
                            return !isNow && trackPathResolved === fading.path ? treeNowFade.level : 0
                        }
                        return hasChildren && !isExpanded && !holdsNow
                            && fading.keys.indexOf(key || "") >= 0
                            ? treeNowFade.level : 0
                    }
                    readonly property string stateGlyph: {
                        if (isNow || holdsNow) {
                            return root.playbackState === "Paused"
                                ? "media-playback-pause" : "media-playback-start"
                        }
                        if (isCurrentTrack) {
                            return "media-playback-stop"
                        }
                        return unreadableReason.length > 0 ? "dialog-warning" : ""
                    }

                    // Where the chevron of this row's level is; a track's
                    // glyph is in its parent's.
                    readonly property real chevronX: root.rowInset + root.indentStep * depthResolved
                    // A row's state glyph is in the chevron column of the
                    // level above: where a track's album has its chevron,
                    // and left of a collapsed album's or artist's own.
                    readonly property real stateX: Math.max(0, chevronX - root.indentStep)
                    readonly property real titleX: isTrackRow
                        ? stateX + root.chevronSize + root.chevronGap + root.thumbnailSize + root.thumbnailGap
                        : chevronX + root.chevronSize + root.chevronGap
                            + (showsThumbnail ? root.thumbnailSize + root.thumbnailGap : 0)

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
                    // The rows below a toggled row over its children; see
                    // the view's transitions.
                    z: 1

                    // A pooled row comes back plain, whatever transition
                    // it left in.
                    ListView.onReused: {
                        libraryRow.z = 1
                        libraryRow.opacity = 1
                    }

                    Accessible.role: Accessible.TreeItem
                    Accessible.name: rowTitle
                    // QML cannot set the expanded state, so the description
                    // tells it, and why an unreadable track is marked.
                    Accessible.description: hasChildren
                        ? (isExpanded
                            ? I18n.i18nc("@info %1 is a library row kind", "%1, expanded", rowKindText)
                            : I18n.i18nc("@info %1 is a library row kind", "%1, collapsed", rowKindText))
                        : (unreadableReason.length > 0
                            ? I18n.i18nc("@info %1 is a library row kind, %2 why its file cannot be read",
                                         "%1, %2", rowKindText, unreadableReason)
                            : rowKindText)
                    Accessible.selectable: true
                    Accessible.selected: rowSelected

                    Components.RowBackground {
                        uiPalette: root.uiPalette
                        opaque: true
                        hovered: libraryRowMouseArea.containsMouse
                        selected: libraryRow.rowSelected
                        active: libraryAlbumView.activeFocus
                        focused: libraryRow.rowCurrent
                    }

                    Kirigami.Icon {
                        id: expanderIcon
                        objectName: "libraryRowChevron"
                        visible: libraryRow.hasChildren
                        x: libraryRow.chevronX
                        y: Math.round((libraryRow.height - height) / 2)
                        width: root.chevronSize
                        height: root.chevronSize
                        source: !libraryRow.hasChildren
                            ? ""
                            : (libraryRow.isExpanded
                                ? "arrow-down"
                                : (Qt.application.layoutDirection === Qt.RightToLeft
                                    ? "arrow-left" : "arrow-right"))
                        isMask: true
                        color: root.uiPalette.textSecondary
                    }

                    // Playing, paused or stopped, or unreadable; a dim
                    // amber mark on a collapsed row holding the playing
                    // track.
                    Kirigami.Icon {
                        objectName: "libraryRowState"
                        readonly property string glyph: libraryRow.stateGlyph
                        visible: glyph.length > 0
                        x: Math.round(libraryRow.stateX + (root.chevronSize - width) / 2)
                        y: Math.round((libraryRow.height - height) / 2)
                        width: root.stateGlyphSize
                        height: root.stateGlyphSize
                        source: glyph
                        isMask: true
                        color: libraryRow.isNow || libraryRow.holdsNow
                            ? root.uiPalette.now
                            : root.uiPalette.textSecondary
                        opacity: libraryRow.holdsNow ? 0.6 : 1

                        ToolTip.visible: stateHover.hovered && libraryRow.unreadableReason.length > 0
                        ToolTip.delay: Kirigami.Units.toolTipDelay
                        ToolTip.text: libraryRow.unreadableReason

                        HoverHandler {
                            id: stateHover
                        }
                    }

                    // The glyph the row had while it was now, or held
                    // what was, fading out over whatever it shows next.
                    Kirigami.Icon {
                        objectName: "libraryRowFadingState"
                        visible: libraryRow.amberLeft > 0
                        x: Math.round(libraryRow.stateX + (root.chevronSize - width) / 2)
                        y: Math.round((libraryRow.height - height) / 2)
                        width: root.stateGlyphSize
                        height: root.stateGlyphSize
                        source: visible ? treeNowFade.target.glyph : ""
                        isMask: true
                        color: root.uiPalette.now
                        opacity: libraryRow.amberLeft * (libraryRow.isTrackRow ? 1 : 0.6)
                    }

                    Kirigami.ShadowedImage {
                        objectName: "libraryRowThumbnail"
                        visible: libraryRow.showsThumbnail
                        x: libraryRow.chevronX + root.chevronSize + root.chevronGap
                        y: Math.round((libraryRow.height - height) / 2)
                        width: root.thumbnailSize
                        height: root.thumbnailSize
                        radius: 2
                        color: "transparent"
                        source: libraryRow.albumCoverInViewport
                            ? root.uiBridge.libraryThumbnailSource(coverPath || "", coverStamp || "")
                            : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: Math.ceil(root.thumbnailSize * Screen.devicePixelRatio)
                        sourceSize.height: Math.ceil(root.thumbnailSize * Screen.devicePixelRatio)
                    }

                    Text {
                        objectName: "libraryRowNumber"
                        visible: libraryRow.isTrackRow && libraryRow.trackNumberResolved > 0
                        x: libraryRow.stateX + root.chevronSize + root.chevronGap
                        width: root.thumbnailSize
                        height: libraryRow.height
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        text: libraryRow.trackNumberResolved > 0 ? String(libraryRow.trackNumberResolved) : ""
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.features: root.uiPalette.tabularFigures
                        color: root.uiPalette.textTertiaryOn(libraryRow.rowSelected)
                    }

                    Text {
                        objectName: "libraryRowTitle"
                        x: libraryRow.titleX
                        width: Math.max(0, yearText.x - x - (yearText.shown ? root.thumbnailGap : 0))
                        height: libraryRow.height
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        text: libraryRow.rowTitle
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.weight: libraryRow.isNow ? Font.DemiBold : Font.Normal
                        readonly property color plainColor: libraryRow.unreadableReason.length > 0
                            ? root.uiPalette.textSecondary
                            : root.uiPalette.text
                        // A collapsed row's title is never amber.
                        color: libraryRow.isNow
                            ? root.uiPalette.now
                            : (libraryRow.isTrackRow && libraryRow.amberLeft > 0
                                ? ColorUtils.mixColor(plainColor, root.uiPalette.now, libraryRow.amberLeft)
                                : plainColor)
                    }

                    Text {
                        id: yearText
                        objectName: "libraryRowYear"
                        // From the row's data, not from `visible`, which
                        // also follows the row's own visibility. A row
                        // without a year has no text, so its width is 0
                        // and the title reaches the right end.
                        readonly property bool shown: libraryRow.yearResolved > 0 && !libraryRow.isTrackRow
                        visible: shown
                        // The right end: the scrollbar lane and the inset.
                        // The width is the implicit one: binding it again
                        // relays out the text, whose implicit width the
                        // binding reads (#474).
                        x: libraryRow.width - Kirigami.Units.largeSpacing - root.rowInset - implicitWidth
                        height: libraryRow.height
                        verticalAlignment: Text.AlignVCenter
                        text: shown ? String(libraryRow.yearResolved) : ""
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        font.features: root.uiPalette.tabularFigures
                        color: root.uiPalette.textTertiaryOn(libraryRow.rowSelected)
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
                        hoverEnabled: true
                        preventStealing: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        drag.target: draggableLibraryItem ? libraryDragProxy : null
                        drag.smoothed: false

                        // Whether a press at `x` is on the chevron.
                        function onChevron(x) {
                            return libraryRow.hasChildren
                                && x >= libraryRow.chevronX - root.chevronGap
                                && x <= libraryRow.chevronX + root.chevronSize + root.chevronGap
                        }

                        onReleased: {
                            libraryDragProxy.x = 0
                            libraryDragProxy.y = 0
                        }

                        onClicked: function(mouse) {
                            libraryAlbumView.forceActiveFocus()
                            const rowMap = libraryRow.actionRowMap()
                            if (mouse.button === Qt.LeftButton && onChevron(mouse.x)) {
                                root.controller.focusRow(libraryRow.selectionKeyResolved)
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
                            if (onChevron(mouse.x)) {
                                root.controller.toggleNode(key)
                                return
                            }
                            root.controller.playRowItems(rowMap)
                        }
                    }
                }

                // One menu for every row, rather than one in each row. Its
                // first items name what they act on: the row, or every
                // selected row when it is one of several.
                Components.AppMenu {
                    id: libraryContextMenu
                    objectName: "libraryContextMenu"
                    property var rowMap: ({})
                    readonly property string rowType: libraryContextMenu.rowMap.rowType || ""
                    readonly property bool actsOnSelection: root.controller.selectionCount() > 1
                        && root.libraryModel.isKeySelected(libraryContextMenu.rowMap.selectionKey || "")

                    Components.KeyHintMenuItem {
                        objectName: "libraryPlayItem"
                        text: {
                            if (libraryContextMenu.actsOnSelection) {
                                return I18n.i18nc("@action:inmenu", "Play Selection")
                            }
                            switch (libraryContextMenu.rowType) {
                            case "artist":
                                return I18n.i18nc("@action:inmenu", "Play Artist")
                            case "album":
                                return I18n.i18nc("@action:inmenu", "Play Album")
                            case "track":
                                return I18n.i18nc("@action:inmenu", "Play Track")
                            default:
                                return I18n.i18nc("@action:inmenu", "Play")
                            }
                        }
                        icon.name: "media-playback-start"
                        // What Enter does in the library.
                        keyHint: I18n.i18nc("@info:shortcut key name", "Enter")
                        enabled: root.controller.isActionableRow(libraryContextMenu.rowMap)
                        onTriggered: root.controller.playRowItems(libraryContextMenu.rowMap)
                    }

                    MenuItem {
                        objectName: "libraryQueueItem"
                        text: {
                            if (libraryContextMenu.actsOnSelection) {
                                return I18n.i18nc("@action:inmenu", "Add Selection to Queue")
                            }
                            switch (libraryContextMenu.rowType) {
                            case "artist":
                                return I18n.i18nc("@action:inmenu", "Add Artist to Queue")
                            case "album":
                                return I18n.i18nc("@action:inmenu", "Add Album to Queue")
                            case "track":
                                return I18n.i18nc("@action:inmenu", "Add Track to Queue")
                            default:
                                return I18n.i18nc("@action:inmenu", "Add to Queue")
                            }
                        }
                        icon.name: "media-playlist-append"
                        enabled: root.controller.isActionableRow(libraryContextMenu.rowMap)
                        onTriggered: root.controller.appendRowItems(libraryContextMenu.rowMap)
                    }

                    MenuSeparator {}

                    // A track's folder shows with the track selected; an
                    // album's or an artist's is the folder itself.
                    MenuItem {
                        objectName: "libraryOpenFolderItem"
                        text: I18n.i18nc("@action:inmenu", "Open Containing Folder")
                        icon.name: "folder-open"
                        enabled: libraryContextMenu.rowType === "track"
                            ? (libraryContextMenu.rowMap.trackPath || "").length > 0
                            : (libraryContextMenu.rowMap.openPath || "").length > 0
                        onTriggered: {
                            if (libraryContextMenu.rowType === "track") {
                                root.uiBridge.openContainingFolder(libraryContextMenu.rowMap.trackPath || "")
                            } else {
                                root.uiBridge.openInFileBrowser(libraryContextMenu.rowMap.openPath || "")
                            }
                        }
                    }

                    MenuItem {
                        objectName: "libraryEditTagsItem"
                        text: I18n.i18nc("@action:inmenu", "Edit Tags…")
                        icon.name: "document-edit"
                        visible: root.controller.canOpenTagEditorForRow(libraryContextMenu.rowMap)
                        enabled: root.controller.canOpenTagEditorForRow(libraryContextMenu.rowMap)
                        onTriggered: root.controller.openTagEditorForRow(libraryContextMenu.rowMap)
                    }

                    MenuSeparator {}

                    // The header has no buttons: collapsing the whole tree
                    // is here, beside the row's own actions.
                    MenuItem {
                        objectName: "libraryCollapseAllItem"
                        text: I18n.i18nc("@action:inmenu", "Collapse All")
                        icon.name: "collapse-all"
                        enabled: root.libraryModel.count > 0
                        onTriggered: root.controller.collapseAll()
                    }
                }
            }

            // With library folders but no tracks yet: one quiet line where
            // the first row would be. Without folders the library shows
            // only its header; the queue explains the first steps.
            Text {
                id: libraryEmptyState
                objectName: "libraryEmptyStateLabel"
                readonly property bool loading: root.controller.isTreeLoading()
                visible: root.uiBridge.libraryRootCount > 0
                    && libraryAlbumView.count === 0
                    && !root.uiBridge.libraryScanInProgress
                x: root.rowInset
                width: Math.max(0, parent.width - 2 * root.rowInset)
                height: root.uiPalette.rowHeight
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: libraryEmptyState.loading
                    ? I18n.i18nc("@info", "Loading library…")
                    : I18n.i18nc("@info", "No music found in the library folders")
                font: root.uiPalette.baseFont
                color: root.uiPalette.textSecondary
            }
        }
    }
}
