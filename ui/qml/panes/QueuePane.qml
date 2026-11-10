// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/KeyUtils.js" as KeyUtils

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
    // The keys of the window's Move Up and Move Down, which the context
    // menu shows beside its own.
    property string moveUpKeyHint: ""
    property string moveDownKeyHint: ""
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property var droppedExternalPaths
    // The tracks the playback failed to read, from the issue history.
    property var errorController: null
    // What the first-run message's button does.
    property var addLibraryFolderAction: null
    // Whether the keyboard is in use, which the current row's ring shows
    // only then. Read as the pane is made, so the app follows the input
    // from the start, before the queue first has the focus.
    readonly property bool keyboardInUse: InputModality.keyboard
    // The widest the queue's minimum width gets while the title alone fits
    // in it: what the narrowest window the design is drawn for, 1000 px,
    // leaves the queue. A larger font drops Artist there rather than push
    // the window wider than the screen.
    property real minimumWidthLimit: Infinity

    // The columns, which the header and the rows share: the state glyph,
    // the number, then Title, Artist, Album and Length. Text that does not
    // fit its column is elided, so it never runs into the next. The title
    // takes the width the others leave, about 40% of the table by default,
    // with Artist and Album about 25% each. Artist, Album and Length have
    // the widths the user dragged their handles to, which the settings
    // keep, or those defaults. A column's width is that of its text; the
    // gaps between the columns come on top. Sizes are those of the design
    // at the default font, and grow with it as the rows do.
    readonly property real sizeScale: root.uiPalette.rowHeight / 26
    // Between the number and the title, and between the other columns.
    readonly property real titleGap: Math.round(12 * root.sizeScale)
    readonly property real columnGap: Math.round(16 * root.sizeScale)
    // From the length's right end to the queue's: the scrollbar lane and
    // the inset.
    readonly property real rightPadding: Math.round(24 * root.sizeScale)
    // The table's share each of Artist and Album has by default.
    readonly property real defaultTextColumnShare: 0.25
    readonly property real defaultArtistColumnWidth: root.defaultTextColumnWidth()
    readonly property real defaultAlbumColumnWidth: root.defaultTextColumnWidth()
    readonly property real defaultLengthColumnWidth: root.minimumLengthColumnWidth
    readonly property real artistColumnWidth: root.columnWidths.artist
    readonly property real albumColumnWidth: root.columnWidths.album
    readonly property real lengthColumnWidth: root.columnWidths.length
    // What the columns other than Title, Artist and Album take, which they
    // have first: the glyph and number columns, Length at its measured
    // width, the gaps before the title and Length, and the right padding.
    readonly property real reservedColumnsWidth: root.playlistIndicatorColumnWidth
        + root.playlistOrderColumnWidth + root.titleGap + root.columnGap
        + root.defaultLengthColumnWidth + root.rightPadding
    // Album drops before the title would go under its minimum with the
    // default widths, as in a narrow window.
    readonly property bool albumColumnVisible: root.artistColumnVisible
        && root.width * (1 - 2 * root.defaultTextColumnShare)
            - root.reservedColumnsWidth >= root.minimumTitleColumnWidth
    // Artist drops next, when what the title leaves would show only a few
    // of its characters, as in a narrow window at a large font.
    readonly property bool artistColumnVisible: root.width - root.reservedColumnsWidth
        - root.minimumTitleColumnWidth - root.columnGap >= root.minimumShownTextColumnWidth
    readonly property var resizableColumns: root.albumColumnVisible
        ? ["artist", "album", "length"]
        : (root.artistColumnVisible ? ["artist", "length"] : ["length"])
    // The narrowest Artist and Album can be dragged: a few characters, and
    // no narrower than the backend keeps. Length keeps room for its
    // heading and the longest length it shows.
    readonly property real minimumResizedColumnWidth: Math.max(
        16, Math.ceil(columnFontMetrics.averageCharacterWidth * 4))
    // The narrowest Artist is shown at.
    readonly property real minimumShownTextColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 6)
    readonly property real minimumLengthColumnWidth: Math.max(16, Math.ceil(Math.max(
        lengthHeaderMetrics.advanceWidth,
        lengthValueMetrics.advanceWidth)))

    // Artist's or Album's default: its share of the table, less the gap
    // before the next column.
    function defaultTextColumnWidth() {
        return Math.max(root.minimumResizedColumnWidth,
                        Math.round(root.width * root.defaultTextColumnShare) - root.columnGap)
    }
    // How far Left or Right moves a focused handle.
    readonly property real keyboardResizeStep: Math.max(
        1, Math.round(columnFontMetrics.averageCharacterWidth))
    // The widest a column is kept, as wide as the backend saves:
    // BinaryBridgeCodec::kMaxQueueColumnWidth.
    readonly property real maximumResizedColumnWidth: 4096
    // The width Artist, Album and Length share, which leaves the title its
    // minimum.
    readonly property real resizableColumnsRoom: Math.max(0, queueHeaderRow.width
        - root.playlistIndicatorColumnWidth - root.playlistOrderColumnWidth
        - root.titleGap - root.minimumTitleColumnWidth
        - root.resizableColumns.length * root.columnGap)
    // The column whose handle is being dragged, or moved with the keys; its
    // preferred width until the drag ends and the width is kept; and, from
    // when the drag started, its preferred width, its edge's distance from
    // the columns' right end (see columnEdgeOffset), how far the drag has
    // moved the edge, and whether the columns were giving way.
    property string resizingColumn: ""
    property real resizingWidth: 0
    property real resizeStartWidth: 0
    property real resizeStartEdge: 0
    property real resizeDelta: 0
    property bool resizeStartedCompressed: false

    function defaultColumnWidth(column) {
        if (column === "artist") {
            return root.defaultArtistColumnWidth
        }
        if (column === "album") {
            return root.defaultAlbumColumnWidth
        }
        return root.defaultLengthColumnWidth
    }

    function minimumColumnWidth(column) {
        return column === "length" ? root.minimumLengthColumnWidth : root.minimumResizedColumnWidth
    }

    function clampColumnWidth(column, width) {
        return Math.max(root.minimumColumnWidth(column),
                        Math.min(root.maximumResizedColumnWidth, width))
    }

    // The width the column would have with room enough: the one the user
    // gave it, or its default.
    function savedColumnWidth(column) {
        const saved = root.uiBridge.queueColumnWidths
            ? Number(root.uiBridge.queueColumnWidths[column])
            : 0
        return saved > 0 ? root.clampColumnWidth(column, saved) : root.defaultColumnWidth(column)
    }

    function preferredColumnWidth(column) {
        return root.resizingColumn === column
            ? root.resizingWidth
            : root.savedColumnWidth(column)
    }

    // The widths the columns are shown at for `preferred` widths. When the
    // queue is too narrow for them with the title at its minimum, as in a
    // window made smaller after a column was widened, each gives up a share
    // of what it has beyond its minimum, down to that minimum.
    function fittedColumnWidths(preferred) {
        const minimum = root.resizableColumns.map(root.minimumColumnWidth)
        const room = root.resizableColumnsRoom
        const total = preferred.reduce((sum, width) => sum + width, 0)
        if (total <= room) {
            return preferred
        }
        const slack = preferred.map((width, i) => Math.max(0, width - minimum[i]))
        const totalSlack = slack.reduce((sum, width) => sum + width, 0)
        if (totalSlack <= 0) {
            return preferred
        }
        const overflow = Math.min(total - room, totalSlack)
        return preferred.map((width, i) => width - overflow * slack[i] / totalSlack)
    }

    readonly property var columnWidths: {
        const widths = root.fittedColumnWidths(root.resizableColumns.map(root.preferredColumnWidth))
        const shown = {}
        root.resizableColumns.forEach((column, i) => shown[column] = Math.round(widths[i]))
        return {
            "artist": shown.artist !== undefined ? shown.artist : 0,
            "album": shown.album !== undefined ? shown.album : 0,
            "length": shown.length
        }
    }

    // How far the left edge of `column`, where its handle is, is from the
    // right end of the columns when it has the preferred width `width` and
    // the others theirs: the shown widths of it and the columns after it.
    function columnEdgeOffset(column, width) {
        const preferred = root.resizableColumns.map(
            name => name === column ? width : root.savedColumnWidth(name))
        return root.fittedColumnWidths(preferred)
            .slice(root.resizableColumns.indexOf(column))
            .reduce((sum, shown) => sum + shown, 0)
    }

    // The preferred width of `column`, from `from` towards `to`, that puts
    // its edge `offset` from the right end, or as near as `to` lets it. The
    // offset never shrinks as the width grows, so halving the range finds
    // the width nearest `from`. Where the edge cannot move, as at the title's
    // minimum, that is `from`, so a drag past it changes nothing.
    function preferredWidthAtEdge(column, offset, from, to) {
        const widening = to > from
        const reachable = root.columnEdgeOffset(column, to)
        const goal = widening ? Math.min(offset, reachable) : Math.max(offset, reachable)
        const reaches = function(width) {
            const edge = root.columnEdgeOffset(column, width)
            return widening ? edge >= goal - 0.25 : edge <= goal + 0.25
        }
        if (reaches(from)) {
            return from
        }
        let near = from
        let far = to
        for (let step = 0; step < 40 && Math.abs(far - near) > 0.25; ++step) {
            const middle = (near + far) / 2
            if (reaches(middle)) {
                far = middle
            } else {
                near = middle
            }
        }
        return far
    }

    function beginColumnResize(column) {
        if (root.resizingColumn !== column) {
            root.finishColumnResize()
            // Read before the column follows resizingWidth.
            const preferred = root.savedColumnWidth(column)
            root.resizeStartEdge = root.columnEdgeOffset(column, preferred)
            root.resizeStartedCompressed = root.resizableColumns
                .map(root.savedColumnWidth)
                .reduce((sum, width) => sum + width, 0) > root.resizableColumnsRoom
            root.resizeStartWidth = preferred
            root.resizingWidth = preferred
            root.resizeDelta = 0
            root.resizingColumn = column
        }
    }

    // Move the column's edge, its handle, `delta` to the left of where the
    // drag started, or as far as the columns let it: the preferred width
    // changes so the edge follows the pointer. A drag that started with room
    // enough stops where the title reaches its minimum; one where the columns
    // gave way takes from the columns after the edge, down to their minimums.
    function resizeColumnBy(column, delta) {
        root.beginColumnResize(column)
        root.resizeDelta = delta
        const start = root.resizeStartWidth
        if (delta === 0) {
            root.resizingWidth = start
            return
        }
        let limit = delta > 0 ? root.maximumResizedColumnWidth : root.minimumColumnWidth(column)
        if (delta > 0 && !root.resizeStartedCompressed) {
            let others = 0
            for (const name of root.resizableColumns) {
                if (name !== column) {
                    others += root.savedColumnWidth(name)
                }
            }
            limit = Math.max(start, Math.min(limit, root.resizableColumnsRoom - others))
        }
        root.resizingWidth = Math.round(root.clampColumnWidth(column,
            root.preferredWidthAtEdge(column, root.resizeStartEdge + delta, start, limit)))
    }

    // Keep the width the drag ended at, once per drag, if it changed.
    function finishColumnResize() {
        const column = root.resizingColumn
        if (column === "") {
            return
        }
        const width = Math.round(root.resizingWidth)
        if (width !== Math.round(root.resizeStartWidth)) {
            root.uiBridge.setQueueColumnWidth(column, width)
        }
        root.resizingColumn = ""
    }

    function cancelColumnResize() {
        root.resizingColumn = ""
    }

    function restoreDefaultColumnWidth(column) {
        root.cancelColumnResize()
        root.uiBridge.setQueueColumnWidth(column, 0)
    }
    // The narrowest the queue gets: every column, and a title column wide
    // enough to tell the tracks apart. The number column is counted for
    // queues of up to 99,999 tracks rather than at its width now, so that
    // the window's minimum does not change as tracks are queued, and the
    // default widths of the others, so that it does not change as they are
    // resized either: narrower windows shrink resized columns.
    // The title's is 240 px at the default font, smaller with a smaller one;
    // a larger font keeps 240 px as long as it holds a dozen characters, so
    // that a narrow window at a large font has room for the other columns.
    readonly property real minimumTitleColumnWidth: Math.ceil(Math.min(240 * root.sizeScale,
        Math.max(240, columnFontMetrics.averageCharacterWidth * 12)))
    readonly property real minimumOrderColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 6.5)
    // Without Album, and the title at its minimum: with Artist at its
    // default share of the width, or, where that is wider than
    // minimumWidthLimit, without Artist, as wide as the limit allows.
    readonly property real minimumTitleOnlyWidth: root.playlistIndicatorColumnWidth
        + root.minimumOrderColumnWidth + root.titleGap + root.minimumTitleColumnWidth
        + root.columnGap + root.defaultLengthColumnWidth + root.rightPadding
    readonly property real minimumContentWidth: Math.ceil(Math.min(
        root.minimumTitleOnlyWidth / (1 - root.defaultTextColumnShare),
        Math.max(root.minimumTitleOnlyWidth, root.minimumWidthLimit)))

    FontMetrics {
        id: columnFontMetrics
        font: root.uiPalette.baseFont
    }

    TextMetrics {
        id: lengthHeaderMetrics
        font: lengthHeader.font
        text: lengthHeader.text
    }

    // The longest length the queue shows: an hour or more.
    TextMetrics {
        id: lengthValueMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
        font.features: root.uiPalette.tabularFigures
        text: "0:00:00"
    }

    // The queue's totals, for its region header.
    readonly property string totalsText: root.uiBridge.queueLength > 0
        ? I18n.i18ncp("@info queue length and total duration", "%1 track · %2", "%1 tracks · %2",
                      root.uiBridge.queueLength, root.uiBridge.queueDurationText || "0:00")
        : ""
    // No library folder and nothing queued: the queue shows the first steps.
    readonly property bool firstRun: root.uiBridge.queueLength === 0
        && root.uiBridge.libraryRootCount === 0

    // A track change: the row that was now fades its amber out as the row
    // that becomes now fades it in, over the same time on the same curve,
    // so no two rows are ever both fully amber (#373 Motion). With nothing
    // fading out, as at the first track, the new row is amber at once.
    // `nowRow` is the row that is amber, as last seen; the playing row's
    // index, path and state change together, and are read together here.
    // A row is matched by its index and its path, so an edit that moves
    // other rows to that index fades nothing, and only a row the queue
    // still has where it was fades.
    property var nowRow: null
    readonly property QtObject nowFade: queueNowFade

    Components.NowFadeOut {
        id: queueNowFade
        fadesIn: true
    }

    function followNowRow() {
        const index = root.uiBridge.playingQueueIndex
        const path = root.uiBridge.currentTrackPath || ""
        const state = root.uiBridge.playbackState || "Stopped"
        const was = root.nowRow
        if (was !== null && (index !== was.index || path !== was.path)
                && root.uiBridge.queuePathAt(was.index) === was.path) {
            root.nowFade.start(was)
        }
        root.nowRow = state !== "Stopped" && index >= 0
            ? {
                index: index,
                path: path,
                glyph: state === "Paused" ? "media-playback-pause" : "media-playback-start"
            }
            : null
    }

    readonly property int playingQueueIndex: root.uiBridge.playingQueueIndex
    readonly property string playingTrackPath: root.uiBridge.currentTrackPath || ""
    readonly property string playbackState: root.uiBridge.playbackState || "Stopped"
    onPlayingQueueIndexChanged: root.followNowRow()
    onPlayingTrackPathChanged: root.followNowRow()
    onPlaybackStateChanged: root.followNowRow()
    Component.onCompleted: root.followNowRow()

    color: root.uiPalette.view
    SplitView.fillWidth: true
    SplitView.preferredHeight: root.preferredHeight
    SplitView.minimumHeight: 220

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The region header: "Queue" and its totals where the Title label
        // would be, then the labels of Artist, Album and Length, with their
        // resize handles between the columns. The glyph and number columns
        // have no label.
        Item {
            id: queueHeader
            objectName: "queueHeader"
            Layout.fillWidth: true
            implicitHeight: root.uiPalette.rowHeight

            Components.RegionHeader {
                anchors.fill: parent
                uiPalette: root.uiPalette
                leftInset: queueHeaderRow.x + titleHeader.x
                rightInset: Math.max(0, parent.width - (queueHeaderRow.x + titleHeader.x + titleHeader.width))
                title: I18n.i18nc("@title queue region", "Queue")
                totals: root.totalsText
            }

            RowLayout {
                id: queueHeaderRow
                anchors.fill: parent
                anchors.rightMargin: root.rightPadding
                // Above the region header's hairline.
                anchors.bottomMargin: 1
                spacing: 0
                // An empty queue shows only its name; the columns keep their
                // places, so nothing moves when tracks arrive.
                readonly property real labelOpacity: root.uiBridge.queueLength > 0 ? 1 : 0

                Item {
                    id: indicatorHeader
                    objectName: "queueHeader_indicator"
                    Layout.preferredWidth: root.playlistIndicatorColumnWidth
                    Layout.fillHeight: true
                    Accessible.role: Accessible.ColumnHeader
                    Accessible.name: I18n.i18nc("@title:column", "Playing")
                }

                Item {
                    id: orderHeader
                    objectName: "queueHeader_order"
                    Layout.preferredWidth: root.playlistOrderColumnWidth
                    Layout.fillHeight: true
                    Accessible.role: Accessible.ColumnHeader
                    Accessible.name: I18n.i18nc("@title:column", "Number")
                }

                // The title column, which the region's name and totals fill.
                Item {
                    id: titleHeader
                    objectName: "queueHeader_title"
                    Layout.leftMargin: root.titleGap
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Accessible.role: Accessible.ColumnHeader
                    Accessible.name: I18n.i18nc("@title:column", "Title")
                }

                Label {
                    id: artistHeader
                    opacity: queueHeaderRow.labelOpacity
                    objectName: "queueHeader_artist"
                    visible: root.artistColumnVisible
                    text: I18n.i18nc("@title:column", "Artist")
                    Layout.leftMargin: root.columnGap
                    Layout.preferredWidth: root.artistColumnWidth
                    elide: Text.ElideRight
                    // Not Qt.font(), which rounds to whole points.
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.smallPointSize
                    color: root.uiPalette.textSecondary
                }
                Label {
                    id: albumHeader
                    opacity: queueHeaderRow.labelOpacity
                    objectName: "queueHeader_album"
                    visible: root.albumColumnVisible
                    text: I18n.i18nc("@title:column", "Album")
                    Layout.leftMargin: root.columnGap
                    Layout.preferredWidth: root.albumColumnWidth
                    elide: Text.ElideRight
                    // Not Qt.font(), which rounds to whole points.
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.smallPointSize
                    color: root.uiPalette.textSecondary
                }
                Label {
                    id: lengthHeader
                    opacity: queueHeaderRow.labelOpacity
                    objectName: "queueHeader_length"
                    elide: Text.ElideRight
                    text: I18n.i18nc("@title:column", "Length")
                    Layout.leftMargin: root.columnGap
                    Layout.preferredWidth: root.lengthColumnWidth
                    horizontalAlignment: Text.AlignRight
                    // Not Qt.font(), which rounds to whole points.
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.smallPointSize
                    color: root.uiPalette.textSecondary
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
                Accessible.name: I18n.i18nc("@label", "Queue")

                // Rows a drag drops elsewhere are where the pointer left
                // them at once, over the rows they displace, which slide
                // to their places. Only a drag's moves slide; the
                // controller tells which made the latest.
                readonly property int slideDuration: Kirigami.Units.shortDuration
                readonly property bool slidesMoves: root.controller.movesSlide
                    && playlistView.slideDuration > 0
                readonly property QtObject slideAnimation: queueSlideAnimation
                move: Transition {
                    objectName: "queueMoveTransition"
                    enabled: playlistView.slidesMoves
                    PropertyAction { property: "z"; value: 2 }
                }
                moveDisplaced: Transition {
                    objectName: "queueMoveDisplacedTransition"
                    enabled: playlistView.slidesMoves
                    SequentialAnimation {
                        PropertyAction { property: "z"; value: 1 }
                        NumberAnimation {
                            id: queueSlideAnimation
                            objectName: "queueMoveDisplacedAnimation"
                            property: "y"
                            duration: playlistView.slideDuration
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                Component.onCompleted: root.controller.registerView(playlistView)
                // The keyboard comes in: a row shows where it is.
                onActiveFocusChanged: {
                    if (playlistView.activeFocus) {
                        root.controller.ensureCurrentRow()
                    }
                }

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

                ScrollBar.vertical: Components.LaneScrollBar {
                    id: playlistVerticalScrollBar
                    objectName: "queueScrollBar"
                    uiPalette: root.uiPalette
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    preventStealing: true
                    onWheel: function(wheel) {
                        root.stepScrollView(playlistView, wheel, playlistView.rowHeight, 3)
                    }
                }

                delegate: Item {
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
                    readonly property string pathValue: rowValues.path || ""
                    readonly property bool isCurrentQueueRow: index === root.uiBridge.playingQueueIndex
                    // Playing or paused: amber marks only what is playing now.
                    readonly property bool isNow: isCurrentQueueRow
                        && root.uiBridge.playbackState !== "Stopped"
                    // The amber left on the row that was now, fading out
                    // after a track change; 0 on every other row.
                    readonly property real amberLeft: {
                        const fading = queueNowFade.target
                        return fading !== null && !isNow && index === fading.index
                            && pathValue === fading.path
                            ? queueNowFade.level : 0
                    }
                    // The share of its amber the row that is now has: less
                    // than 1 only while it takes it over from the row that
                    // was now.
                    readonly property real amberIn: isNow ? queueNowFade.levelIn : 0
                    readonly property string unreadableReason: root.errorController
                        && pathValue.length > 0
                        ? (root.errorController.unreadablePaths[pathValue] || "")
                        : ""
                    readonly property bool unreadable: unreadableReason.length > 0
                    readonly property bool draggableQueueItem: true
                    readonly property int queueRowIndex: index
                    readonly property bool rowSelected: root.controller.isIndexSelected(index)
                    // The row the keyboard is on, in a queue with focus,
                    // while the keyboard is in use: a click selects with the
                    // tint alone.
                    readonly property bool rowCurrent: playlistView.activeFocus
                        && root.keyboardInUse
                        && index === root.controller.currentRowIndex
                    readonly property string playbackStateText: !isCurrentQueueRow
                        ? ""
                        : (root.uiBridge.playbackState === "Paused"
                            ? I18n.i18nc("@info accessible state of the current queue row", "paused")
                            : (root.uiBridge.playbackState === "Stopped"
                                ? I18n.i18nc("@info accessible state of the current queue row", "stopped")
                                : I18n.i18nc("@info accessible state of the current queue row", "playing")))
                    readonly property string stateGlyph: isCurrentQueueRow
                        ? (root.uiBridge.playbackState === "Paused"
                            ? "media-playback-pause"
                            : (root.uiBridge.playbackState === "Stopped"
                                ? "media-playback-stop" : "media-playback-start"))
                        : (unreadable ? "dialog-warning" : "")

                    width: ListView.view.width
                    height: playlistView.rowHeight
                    // Moved rows over the rows they displace; see the
                    // view's transitions.
                    z: 1

                    Accessible.role: Accessible.ListItem
                    Accessible.name: [
                        root.playlistOrderText(index) + ".",
                        titleValue,
                        artistValue,
                        albumValue,
                        lengthTextValue,
                        playbackStateText,
                        unreadableReason
                    ].filter(function(part) { return part.length > 0 }).join(", ")
                    Accessible.selectable: true
                    Accessible.selected: rowSelected

                    Components.RowBackground {
                        uiPalette: root.uiPalette
                        opaque: true
                        hovered: playlistRowMouseArea.containsMouse
                        selected: playlistRow.rowSelected
                        active: playlistView.activeFocus
                        focused: playlistRow.rowCurrent
                    }

                    // The cells are where the header's columns are, which
                    // the header lays out for every row; plain Text and
                    // icons in the shared palette's colours. They sit on
                    // whole pixels.
                    readonly property real cellY: Math.round((playlistRow.height - titleCell.implicitHeight) / 2)
                    readonly property color columnTextColor: playlistRow.unreadable
                        ? root.uiPalette.textSecondary
                        : root.uiPalette.textColumn

                    Item {
                        objectName: "queueCell_indicator"
                        x: queueHeaderRow.x + indicatorHeader.x
                        width: indicatorHeader.width
                        height: playlistRow.height

                        Kirigami.Icon {
                            objectName: "queueRowState"
                            readonly property string glyph: playlistRow.stateGlyph
                            visible: glyph.length > 0
                            anchors.centerIn: parent
                            width: root.uiPalette.scaledSize(12)
                            height: root.uiPalette.scaledSize(12)
                            source: glyph
                            isMask: true
                            color: playlistRow.isNow ? root.uiPalette.now : root.uiPalette.textSecondary
                            // The amber glyph fades in as the old row's
                            // fades out.
                            opacity: playlistRow.isNow ? playlistRow.amberIn : 1

                            ToolTip.visible: stateHover.hovered && playlistRow.unreadable
                            ToolTip.delay: Kirigami.Units.toolTipDelay
                            ToolTip.text: playlistRow.unreadableReason

                            HoverHandler {
                                id: stateHover
                            }
                        }

                        // The glyph the row had while it was now, fading
                        // out over whatever it shows next.
                        Kirigami.Icon {
                            objectName: "queueRowFadingState"
                            visible: playlistRow.amberLeft > 0
                            anchors.centerIn: parent
                            width: root.uiPalette.scaledSize(12)
                            height: root.uiPalette.scaledSize(12)
                            source: visible ? queueNowFade.target.glyph : ""
                            isMask: true
                            color: root.uiPalette.now
                            opacity: playlistRow.amberLeft
                        }
                    }

                    Text {
                        objectName: "queueCell_order"
                        x: queueHeaderRow.x + orderHeader.x
                        y: playlistRow.cellY
                        width: orderHeader.width
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.features: root.uiPalette.tabularFigures
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignRight
                        text: root.playlistOrderText(index)
                        color: root.uiPalette.textTertiaryOn(playlistRow.rowSelected)
                    }

                    Text {
                        id: titleCell
                        objectName: "queueCell_title"
                        x: queueHeaderRow.x + titleHeader.x
                        y: playlistRow.cellY
                        width: titleHeader.width
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignLeft
                        text: playlistRow.titleValue
                        readonly property color plainColor: playlistRow.unreadable
                            ? root.uiPalette.textSecondary : root.uiPalette.text
                        // The title's share of amber, taken in by the row
                        // that becomes now as the row that was now gives it
                        // up. It sets the weight and the colour together, so
                        // the two change in the same frame: semibold while
                        // the title is mostly amber, which the two rows swap
                        // at the fade's midpoint.
                        readonly property real amber: playlistRow.isNow
                            ? playlistRow.amberIn : playlistRow.amberLeft
                        font.weight: amber >= 0.5 ? Font.DemiBold : Font.Normal
                        color: amber >= 1
                            ? root.uiPalette.now
                            : (amber > 0 ? ColorUtils.mixColor(plainColor, root.uiPalette.now, amber) : plainColor)
                    }

                    Text {
                        objectName: "queueCell_artist"
                        visible: artistHeader.visible
                        x: queueHeaderRow.x + artistHeader.x
                        y: playlistRow.cellY
                        width: artistHeader.width
                        font: root.uiPalette.baseFont
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignLeft
                        text: playlistRow.artistValue
                        color: playlistRow.columnTextColor
                    }

                    Text {
                        objectName: "queueCell_album"
                        visible: albumHeader.visible
                        x: queueHeaderRow.x + albumHeader.x
                        y: playlistRow.cellY
                        width: albumHeader.width
                        font: root.uiPalette.baseFont
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignLeft
                        text: playlistRow.albumValue
                        color: playlistRow.columnTextColor
                    }

                    Text {
                        objectName: "queueCell_length"
                        x: queueHeaderRow.x + lengthHeader.x
                        y: playlistRow.cellY
                        width: lengthHeader.width
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.features: root.uiPalette.tabularFigures
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignRight
                        text: playlistRow.lengthTextValue
                        color: root.uiPalette.textSecondary
                    }

                    MouseArea {
                        id: playlistRowMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
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

                // The keys beside the items are what the queue does with them;
                // see KeyHintMenuItem.
                Components.AppMenu {
                    id: playlistContextMenu
                    objectName: "playlistContextMenu"
                    property int rowIndex: -1

                    Components.KeyHintMenuItem {
                        objectName: "queuePlayItem"
                        text: I18n.i18nc("@action:inmenu", "Play")
                        icon.name: "media-playback-start"
                        keyHint: I18n.i18nc("@info:shortcut key name", "Enter")
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            if (playlistContextMenu.rowIndex >= 0) {
                                root.controller.playAt(playlistContextMenu.rowIndex)
                            }
                        }
                    }

                    MenuSeparator {}

                    MenuItem {
                        objectName: "queueShowInLibraryItem"
                        text: I18n.i18nc("@action:inmenu", "Show in Library")
                        icon.name: "go-jump"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            const path = root.uiBridge.queuePathAt(playlistContextMenu.rowIndex)
                            root.libraryController.showTrackInLibrary(path || "")
                        }
                    }

                    MenuItem {
                        objectName: "queueOpenFolderItem"
                        text: I18n.i18nc("@action:inmenu", "Open Containing Folder")
                        icon.name: "folder-open"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            const path = root.uiBridge.queuePathAt(playlistContextMenu.rowIndex)
                            if (path && path.length > 0) {
                                root.uiBridge.openContainingFolder(path)
                            }
                        }
                    }

                    MenuItem {
                        objectName: "queueEditTagsItem"
                        text: I18n.i18nc("@action:inmenu", "Edit Tags…")
                        icon.name: "document-edit"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: root.controller.openTagEditorForRow(playlistContextMenu.rowIndex)
                    }

                    MenuSeparator {}

                    Components.KeyHintMenuItem {
                        objectName: "queueMoveUpItem"
                        text: I18n.i18nc("@action:inmenu", "Move Up")
                        icon.name: "go-up"
                        keyHint: root.moveUpKeyHint
                        enabled: playlistContextMenu.rowIndex > 0
                        onTriggered: root.controller.moveRowsOf(playlistContextMenu.rowIndex, -1)
                    }

                    Components.KeyHintMenuItem {
                        objectName: "queueMoveDownItem"
                        text: I18n.i18nc("@action:inmenu", "Move Down")
                        icon.name: "go-down"
                        keyHint: root.moveDownKeyHint
                        enabled: playlistContextMenu.rowIndex >= 0
                            && playlistContextMenu.rowIndex < root.uiBridge.queueLength - 1
                        onTriggered: root.controller.moveRowsOf(playlistContextMenu.rowIndex, 1)
                    }

                    Components.KeyHintMenuItem {
                        objectName: "queueRemoveItem"
                        text: I18n.i18nc("@action:inmenu", "Remove from Queue")
                        icon.name: "list-remove"
                        keyHint: I18n.i18nc("@info:shortcut key name", "Del")
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
                }
            }

            // First run, with no library folder: the window's one message.
            ColumnLayout {
                id: firstRunMessage
                objectName: "queueFirstRunMessage"
                visible: root.firstRun
                anchors.centerIn: parent
                width: Math.min(parent.width - 2 * Kirigami.Units.gridUnit,
                                Math.max(implicitWidth, Kirigami.Units.gridUnit * 20))
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    objectName: "queueFirstRunIcon"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: Kirigami.Units.iconSizes.large
                    Layout.preferredHeight: Kirigami.Units.iconSizes.large
                    // The app icon's own 48 px drawing, built in.
                    source: "qrc:/icons/hicolor/48x48/apps/ferrous.svg"
                }

                Text {
                    objectName: "queueFirstRunTitle"
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: I18n.i18nc("@info:placeholder", "Start with your music folder")
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.basePointSize * 15 / 13
                    font.weight: Font.DemiBold
                    color: root.uiPalette.text
                }

                Text {
                    objectName: "queueFirstRunExplanation"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: I18n.i18nc("@info:placeholder",
                        "Ferrous reads the tags and watches the folder, so new music appears on its own.\nYou can also drop audio files here.")
                    font: root.uiPalette.baseFont
                    color: root.uiPalette.textSecondary
                }

                Button {
                    objectName: "firstRunAddFolderButton"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    visible: root.addLibraryFolderAction !== null
                    action: root.addLibraryFolderAction
                    highlighted: true
                }
            }

            // Library folders but nothing queued: what fills the queue, as
            // the first-run message says its first step, with no glyph and
            // no button.
            // It keeps 24 px clear of the pane's sides, and its lines wrap
            // to that width.
            ColumnLayout {
                id: emptyQueueMessage
                objectName: "queueEmptyMessage"
                visible: root.uiBridge.queueLength === 0 && !root.firstRun
                anchors.centerIn: parent
                width: Math.max(0, Math.min(parent.width - 6 * Kirigami.Units.largeSpacing,
                                            Math.max(implicitWidth, Kirigami.Units.gridUnit * 20)))
                spacing: Kirigami.Units.smallSpacing

                Text {
                    objectName: "queueEmptyLabel"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: I18n.i18nc("@info:placeholder", "The queue is empty")
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.basePointSize * 15 / 13
                    font.weight: Font.DemiBold
                    color: root.uiPalette.text
                }

                Text {
                    objectName: "queueEmptyExplanation"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: I18n.i18nc("@info:placeholder",
                        "Double-click music in the library to play it, or drag it here.")
                    font: root.uiPalette.baseFont
                    color: root.uiPalette.textSecondary
                }
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

            // Where a drop lands: a 2 px line in the focus colour between
            // the rows, as wide as their tint.
            Rectangle {
                objectName: "queueDropIndicator"
                x: playlistView.x
                width: Math.max(0, playlistView.width - Kirigami.Units.largeSpacing)
                height: 2
                y: playlistDropArea.queueDropIndicatorY
                color: root.uiPalette.focusRing
                visible: playlistDropArea.containsDrag
                    && playlistDropArea.queueDropAccepted
                    && playlistDropArea.queueDropInsertIndex >= 0
            }
        }
    }

    // The resize handles over the header. They come after the list in the
    // item tree, which orders the Tab chain, so Tab goes from the library
    // to the queue's rows, and on to the handles only after them.
    Item {
        objectName: "queueColumnHandles"
        // The header's place: the column layout fills the pane.
        x: queueHeader.x
        y: queueHeader.y
        width: queueHeader.width
        height: queueHeader.height

        // A handle on the left of each resizable column, in the gap
        // before it: the title fills the width the others leave, so
        // dragging it left widens the column after it. Double-click,
        // or Home, restores the column's default width; Left and Right
        // move it from the keyboard. It shows only under the pointer
        // and with keyboard focus.
        Repeater {
            model: [
                { "column": "artist", "label": I18n.i18nc("@title:column", "Artist"), "header": artistHeader },
                { "column": "album", "label": I18n.i18nc("@title:column", "Album"), "header": albumHeader },
                { "column": "length", "label": I18n.i18nc("@title:column", "Length"), "header": lengthHeader }
            ]

            delegate: Item {
                id: resizeHandle
                objectName: "queueColumnHandle_" + modelData.column
                required property var modelData
                readonly property string column: modelData.column
                readonly property bool highlighted: resizeMouseArea.containsMouse
                    || resizeMouseArea.pressed || resizeHandle.activeFocus
                // Where the pointer was when the drag started, in the
                // pane, which stays put as the columns move.
                property real pressX: 0

                visible: modelData.header.visible
                x: queueHeaderRow.x + modelData.header.x - root.columnGap / 2 - width / 2
                width: Kirigami.Units.largeSpacing
                height: parent.height
                // An empty queue's columns have no labels to resize.
                activeFocusOnTab: visible && root.uiBridge.queueLength > 0

                Accessible.role: Accessible.Separator
                Accessible.name: I18n.i18nc("@action %1 is a column title", "Resize the %1 column", modelData.label)
                Accessible.description: I18n.i18nc("@info", "Left or Right to resize, Home for the default width")
                Accessible.focusable: true

                Rectangle {
                    objectName: "queueColumnHandleLine"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.topMargin: Kirigami.Units.smallSpacing
                    anchors.bottomMargin: Kirigami.Units.smallSpacing
                    width: resizeHandle.activeFocus ? 2 : 1
                    visible: resizeHandle.highlighted
                    color: resizeHandle.activeFocus || resizeMouseArea.pressed
                        ? root.uiPalette.focusRing
                        : root.uiPalette.seam
                }

                MouseArea {
                    id: resizeMouseArea
                    anchors.fill: parent
                    hoverEnabled: root.uiBridge.queueLength > 0
                    preventStealing: true
                    cursorShape: Qt.SplitHCursor

                    onPressed: function(mouse) {
                        resizeHandle.pressX = mapToItem(root, mouse.x, 0).x
                        root.beginColumnResize(resizeHandle.column)
                    }
                    onPositionChanged: function(mouse) {
                        if (pressed && root.resizingColumn === resizeHandle.column) {
                            const x = mapToItem(root, mouse.x, 0).x
                            root.resizeColumnBy(resizeHandle.column, resizeHandle.pressX - x)
                        }
                    }
                    onReleased: root.finishColumnResize()
                    onCanceled: root.cancelColumnResize()
                    onDoubleClicked: root.restoreDefaultColumnWidth(resizeHandle.column)
                }

                function keyResizes(event) {
                    return KeyUtils.isUnmodified(event)
                        && (event.key === Qt.Key_Left || event.key === Qt.Key_Right
                            || event.key === Qt.Key_Home)
                }
                // The keys move the focused handle rather than seeking.
                Keys.onShortcutOverride: function(event) {
                    event.accepted = resizeHandle.keyResizes(event)
                }
                Keys.onPressed: function(event) {
                    if (!resizeHandle.keyResizes(event)) {
                        return
                    }
                    event.accepted = true
                    if (event.key === Qt.Key_Home) {
                        root.restoreDefaultColumnWidth(resizeHandle.column)
                        return
                    }
                    root.beginColumnResize(resizeHandle.column)
                    root.resizeColumnBy(resizeHandle.column, root.resizeDelta
                        + (event.key === Qt.Key_Left ? root.keyboardResizeStep : -root.keyboardResizeStep))
                }
                // A held key moves the handle until it is let go, when
                // the width is kept.
                Keys.onReleased: function(event) {
                    if (!event.isAutoRepeat && resizeHandle.keyResizes(event)) {
                        event.accepted = true
                        root.finishColumnResize()
                    }
                }
                onActiveFocusChanged: {
                    if (!resizeHandle.activeFocus && !resizeMouseArea.pressed) {
                        root.finishColumnResize()
                    }
                }
            }
        }
    }
}
