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
    // fit its column is elided, so it never runs into the next. Length has
    // its room first, as wide as its longest value and a little more; of
    // the room left for the text, the title takes what the others leave,
    // a little under half by default, with Artist a little over a quarter
    // and Album a little less, as album names run shorter. Artist, Album
    // and Length have
    // the widths the user dragged their handles to, which the settings
    // keep, or those defaults. A column's width is that of its text; the
    // gaps between the columns come on top. Sizes are those of the design
    // at the default font, and grow with it as the rows do.
    readonly property real sizeScale: root.uiPalette.rowHeight / 26
    // An unreadable row's mark: an error's ring with a cross, at the
    // status bar's size, in the negative colour, as the status bar and
    // Recent Issues mark the same failure; the triangle means a warning
    // everywhere (the row had it, in red, for an error).
    readonly property string errorGlyph: Qt.resolvedUrl("../assets/status-error.svg")
    readonly property int errorGlyphSize: root.uiPalette.scaledSize(16)
    // Between the number and the title, and between the other columns.
    readonly property real titleGap: root.compactColumns ? root.compactGap : root.standardTitleGap
    readonly property real columnGap: root.compactColumns ? root.compactGap : root.standardColumnGap
    readonly property real standardTitleGap: Math.round(12 * root.sizeScale)
    readonly property real standardColumnGap: Math.round(16 * root.sizeScale)
    // The state glyph's and the number's columns, which the header and the
    // rows share.
    readonly property real indicatorColumnWidth: root.compactColumns
        ? root.compactIndicatorColumnWidth : root.playlistIndicatorColumnWidth
    readonly property real orderColumnWidth: root.compactColumns
        ? root.compactOrderColumnWidth : root.playlistOrderColumnWidth
    // The narrow queue at a large font, where the gaps and the columns
    // of the font would leave Artist less than its sixteen characters (at
    // 24 pt in 1000 px the state glyph, the number and the gap before the
    // title took 132 px, for a one-digit number, and Artist showed four
    // letters of a name beside elided titles). There the columns are
    // compact, and the room goes to the names, the title's first: the
    // state glyph's column is the error mark's, the largest, with 2 px at
    // either side; the number's is its widest, of at least two digits,
    // with 6 px before it; the gaps are 12 px and Length has its least
    // width. The title keeps its usual minimum, and Artist shows where
    // what is left holds ten characters of a name, and no fewer than
    // 120 px; otherwise the title has the row (at 120 px Artist showed
    // "The Q…" on every row). At the default font the columns are the
    // design's, so it is only for a larger one.
    readonly property bool compactColumns: root.uiPalette.fontScale > 1
        && root.standardArtistRoom < root.minimumArtistColumnWidth
    readonly property real compactGap: Math.min(12, root.standardColumnGap)
    readonly property real compactIndicatorColumnWidth: Math.min(root.playlistIndicatorColumnWidth,
        root.errorGlyphSize + 4)
    readonly property real compactOrderColumnWidth: Math.min(root.playlistOrderColumnWidth,
        Math.ceil(widestOrderMetrics.advanceWidth) + 6)
    readonly property real compactMinimumArtistColumnWidth: Math.max(120, root.minimumShownTextColumnWidth,
        Math.ceil(columnFontMetrics.averageCharacterWidth * 10))
    readonly property bool compactArtistFits: root.width - root.compactIndicatorColumnWidth
        - root.compactOrderColumnWidth - 3 * root.compactGap - root.minimumTitleColumnWidth
        - root.compactMinimumArtistColumnWidth - root.minimumLengthColumnWidth - root.rightPadding >= 0
    // What the title at its minimum leaves Artist with the columns and
    // gaps of the font.
    readonly property real standardArtistRoom: root.width - root.standardReservedColumnsWidth
        - root.minimumTitleColumnWidth - root.standardColumnGap
    // From the length's right end to the queue's: the scrollbar's lane,
    // which keeps its 8 px at any font, and a 16 px inset that grows with
    // the font to half as much again, 24 px at 24 pt. Scaled as one, the
    // two made 48 px at 24 pt, a gutter twice the lane's beside a column
    // set that dropped Album and Artist for want of room.
    readonly property real rightPadding: playlistVerticalScrollBar.lane
        + Math.round(16 * Math.min(root.sizeScale, 1.5))
    // The share of the text columns' room Artist and Album have by default;
    // the title has the rest, 45%. Half left a gap after short titles and
    // elided artists at a large font while the title had room; these give
    // Artist and Album about 24 px more each at 1600 px.
    readonly property real defaultArtistColumnShare: 0.283
    readonly property real defaultAlbumColumnShare: 0.263
    readonly property real defaultTitleColumnShare: 1 - root.defaultArtistColumnShare - root.defaultAlbumColumnShare
    // What Title, Artist and Album share: the table, less the columns that
    // have their room first and the gaps before Artist and Album.
    readonly property real defaultTextColumnsRoom: Math.max(0,
        root.width - root.reservedColumnsWidth - 2 * root.columnGap)
    // Artist by default: its share, or, where that is narrower, as with a
    // large font, minimumArtistColumnWidth, which the title gives way to
    // as far as its own minimum (at 24 pt Artist showed eleven
    // characters of a name beside a title column twice as wide as its
    // titles).
    // The compact columns keep Artist its least there.
    readonly property real defaultArtistColumnWidth: Math.max(
        root.defaultTextColumnWidth(root.defaultArtistColumnShare),
        Math.min(root.minimumArtistColumnWidth, root.artistRoomAtTitleMinimum),
        root.compactColumns ? root.compactMinimumArtistColumnWidth : 0)
    // The width Artist takes from the title before Artist's share: sixteen
    // characters of the font, as many as most artists' names have; under
    // its share with the default font but in the narrowest windows.
    readonly property real minimumArtistColumnWidth: Math.ceil(columnFontMetrics.averageCharacterWidth * 16)
    // What the title leaves Artist at the title's minimum, with Album and
    // Length at their defaults.
    readonly property real artistRoomAtTitleMinimum: Math.floor(root.width - root.reservedColumnsWidth
        - root.minimumTitleColumnWidth - root.columnGap
        - (root.albumColumnVisible ? root.columnGap + root.defaultAlbumColumnWidth : 0))
    readonly property real defaultAlbumColumnWidth: root.defaultTextColumnWidth(root.defaultAlbumColumnShare)
    // Length: its longest value and two characters' room before it, apart
    // from the album; about 56 px at the default font. The compact
    // columns give Length its least width.
    readonly property real measuredLengthColumnWidth: Math.max(root.minimumLengthColumnWidth,
        Math.ceil(lengthValueMetrics.advanceWidth + 2 * columnFontMetrics.averageCharacterWidth))
    readonly property real defaultLengthColumnWidth: root.compactColumns
        ? root.minimumLengthColumnWidth : root.measuredLengthColumnWidth
    readonly property real artistColumnWidth: root.columnWidths.artist
    readonly property real albumColumnWidth: root.columnWidths.album
    readonly property real lengthColumnWidth: root.columnWidths.length
    // What the columns other than Title, Artist and Album take, which they
    // have first: the glyph and number columns, Length at its measured
    // width, the gaps before the title and Length, and the right padding.
    readonly property real reservedColumnsWidth: root.indicatorColumnWidth
        + root.orderColumnWidth + root.titleGap + root.columnGap
        + root.defaultLengthColumnWidth + root.rightPadding
    // The same with the gaps and Length of the font, which decide which
    // columns show.
    readonly property real standardReservedColumnsWidth: root.playlistIndicatorColumnWidth
        + root.playlistOrderColumnWidth + root.standardTitleGap + root.standardColumnGap
        + root.measuredLengthColumnWidth + root.rightPadding
    // Album drops first, as in a narrow window: it shows only where half
    // the table, less the columns that have their room first, would still
    // hold the title at its minimum, so a narrow window gives the title
    // the room rather than three cramped columns.
    readonly property bool albumColumnVisible: !root.compactColumns
        && root.standardArtistRoom >= root.minimumShownTextColumnWidth
        && root.width / 2 - root.standardReservedColumnsWidth >= root.minimumTitleColumnWidth
    // Artist drops next, when what the title leaves would show only a few
    // of its characters, as in a narrow window, or, with the compact
    // columns, fewer than ten.
    readonly property bool artistColumnVisible: root.compactColumns
        ? root.compactArtistFits
        : root.standardArtistRoom >= root.minimumShownTextColumnWidth
    // The least width at which the queue shows all its columns: Album,
    // the first to drop, shows from here (albumColumnVisible). The number
    // column is counted as the queue's minimum counts it, for up to 99,999
    // tracks, so that it does not change as tracks are queued. The window
    // gives the library pane more than its share only past this.
    readonly property real widthWithAllColumns: Math.ceil(2 * (root.playlistIndicatorColumnWidth
        + root.minimumOrderColumnWidth + root.standardTitleGap + root.standardColumnGap + root.measuredLengthColumnWidth
        + root.rightPadding + root.minimumTitleColumnWidth))
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

    // Artist's or Album's default: its share of the text columns' room.
    function defaultTextColumnWidth(share) {
        return Math.max(root.minimumResizedColumnWidth, Math.round(root.defaultTextColumnsRoom * share))
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
        - root.indicatorColumnWidth - root.orderColumnWidth
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
    // Without Album, and the title at its minimum: with Artist at
    // minimumArtistColumnShare of the width and Length at its default, or, where that
    // is wider than minimumWidthLimit, without Artist, as wide as the limit
    // allows. Without Artist, Length counts at its least width, its heading
    // or its longest value: in the narrowest queue it gives up the two
    // characters' room before it, as the resized columns give up theirs,
    // so that the narrow window at a large font, with the wider sidebar,
    // keeps the title at its minimum.
    readonly property real minimumTitleOnlyWidth: root.playlistIndicatorColumnWidth
        + root.minimumOrderColumnWidth + root.standardTitleGap + root.minimumTitleColumnWidth
        + root.standardColumnGap + root.minimumLengthColumnWidth + root.rightPadding
    readonly property real minimumWithArtistWidth: (root.minimumTitleOnlyWidth
        - root.minimumLengthColumnWidth + root.measuredLengthColumnWidth)
        / (1 - root.minimumArtistColumnShare)
    // The share of the width the narrowest queue is counted with for Artist:
    // the one it had before its default grew, so the window's
    // minimum width stays. There Artist gives way to the title's minimum.
    readonly property real minimumArtistColumnShare: 0.26
    readonly property real minimumContentWidth: Math.ceil(Math.min(
        root.minimumWithArtistWidth,
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

    // The widest number the queue shows, of two digits at least, as the
    // rows draw it.
    TextMetrics {
        id: widestOrderMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
        font.features: root.uiPalette.tabularFigures
        text: root.playlistOrderText(Math.max(9, root.uiBridge.queueLength - 1))
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
    // Until the backend has read the library, no folder is not yet known,
    // and the queue shows neither message.
    readonly property bool libraryLoaded: root.uiBridge.libraryLoaded === true
    readonly property bool firstRun: root.libraryLoaded
        && root.uiBridge.queueLength === 0
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

    // Where the list starts in the pane, and the height of its rows: the
    // default split puts the list on their grid.
    readonly property real listTop: queueHeader.implicitHeight
    readonly property real rowPitch: playlistView.rowHeight

    color: root.uiPalette.view
    SplitView.fillWidth: true
    SplitView.preferredHeight: root.preferredHeight
    // Its header and four rows, and never under 220 px: about eight rows
    // with the default font, and five at 24 pt, where the window's least
    // height, 720 px, still has room for the visualization's minimum.
    SplitView.minimumHeight: Math.max(220, 5 * root.uiPalette.rowHeight)

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
                objectName: "queueRegionHeader"
                anchors.fill: parent
                uiPalette: root.uiPalette
                // Over the Title column, with rows or without: it is that
                // column's heading, and stays as tracks come (at the panes'
                // 12 px inset while empty, it jumped 54 px on the first
                // Play).
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
                    Layout.preferredWidth: root.indicatorColumnWidth
                    Layout.fillHeight: true
                    Accessible.role: Accessible.ColumnHeader
                    Accessible.name: I18n.i18nc("@title:column", "Playing")
                }

                Item {
                    id: orderHeader
                    objectName: "queueHeader_order"
                    Layout.preferredWidth: root.orderColumnWidth
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
                    textFormat: Text.PlainText
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
                    textFormat: Text.PlainText
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
                    textFormat: Text.PlainText
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
                        playlistContextMenu.popupAt(row, 0, row.height)
                    } else {
                        playlistContextMenu.popupAt(playlistView, 0, 0)
                    }
                }

                ScrollBar.vertical: Components.LaneScrollBar {
                    id: playlistVerticalScrollBar
                    objectName: "queueScrollBar"
                    uiPalette: root.uiPalette
                }
                // Where the rows' states end on the right while the
                // scrollbar shows: its lane and 2 px more, so the focus
                // ring, which the tint ends with, keeps 3 px clear of the
                // thumb at its widest (at the lane, 1 px off it, the two
                // read as touching). Without it, at the pane's edge.
                readonly property real rowStatesEnd: playlistVerticalScrollBar.visible
                    ? playlistVerticalScrollBar.lane + 2 : 0
                // The row under the pointer, if any: while it shows why it
                // is unreadable, the keyboard's row does not, so the list
                // gives one reason at a time.
                readonly property Item pointedRow: playlistPointer.hovered
                    ? playlistView.itemAt(playlistPointer.point.position.x + playlistView.contentX,
                                          playlistPointer.point.position.y + playlistView.contentY)
                    : null

                HoverHandler {
                    id: playlistPointer
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
                    readonly property string lengthTextValue: rowValues.lengthText
                        || I18n.i18nc("@info:status no time", "–:––")
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
                    // Why the row is unreadable shows for the pointer
                    // anywhere on it, unless it holds the row down, and
                    // for the keyboard on it, unless the pointer shows
                    // another row's.
                    readonly property bool reasonPointed: unreadable
                        && playlistRowMouseArea.containsMouse
                        && playlistRowMouseArea.pressedButtons === Qt.NoButton
                    readonly property bool reasonShown: reasonPointed
                        || (unreadable && rowCurrent
                            && !(playlistView.pointedRow !== null && playlistView.pointedRow !== playlistRow
                                 && playlistView.pointedRow.reasonPointed))
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
                    // Playing or paused, what the playback does; otherwise
                    // the error mark of an unreadable file before the stop
                    // mark, as a file that fails to play stays the
                    // current row, stopped, and the error says why
                    // (it showed the stop mark, with the
                    // error as its tooltip, until another row played).
                    readonly property bool showsError: !isNow && unreadable
                    readonly property string stateGlyph: isNow
                        ? (root.uiBridge.playbackState === "Paused"
                            ? "media-playback-pause" : "media-playback-start")
                        : (showsError
                            ? root.errorGlyph
                            : (isCurrentQueueRow ? "media-playback-stop" : ""))

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
                        hoverColor: root.uiPalette.rowHover
                        selected: playlistRow.rowSelected
                        // A selected row answers the pointer too, and
                        // its press, until a drag takes the row away.
                        selectedHover: true
                        pressed: (playlistRowMouseArea.pressedButtons & Qt.LeftButton) !== 0
                            && !playlistRowMouseArea.dragCancelled
                            && playlistView.draggingRow !== playlistRowMouseArea
                        active: playlistView.activeFocus
                        focused: playlistRow.rowCurrent
                        // Every state ends clear of the scrollbar's lane
                        // while it shows, and of its thumb.
                        scrollbarLane: playlistView.rowStatesEnd
                    }

                    // The cells are where the header's columns are, which
                    // the header lays out for every row; plain Text and
                    // icons in the shared palette's colours. They sit on
                    // whole pixels.
                    readonly property real cellY: Math.round((playlistRow.height - titleCell.implicitHeight) / 2)
                    readonly property color secondaryTextColor: playlistRow.rowSelected
                        ? root.uiPalette.textSecondarySelected
                        : root.uiPalette.textSecondary
                    readonly property color columnTextColor: playlistRow.unreadable
                        ? playlistRow.secondaryTextColor
                        : playlistRow.rowSelected ? root.uiPalette.textColumnSelected
                        : root.uiPalette.textColumn
                    // The amber of what plays, made readable on the
                    // selection as well.
                    readonly property color nowColor: playlistRow.rowSelected
                        ? root.uiPalette.nowSelected
                        : root.uiPalette.now

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
                            // The error mark at the status bar's size, the
                            // transport's marks smaller, and the stop
                            // square smaller again, so that its solid ink
                            // does not outweigh the play and pause marks
                            // (at their size it read heaviest of the three).
                            width: playlistRow.showsError ? root.errorGlyphSize
                                : glyph === "media-playback-stop" ? root.uiPalette.scaledSize(10)
                                : root.uiPalette.scaledSize(12)
                            height: width
                            // At its size, which grows with the font
                            // (Kirigami drew the 29 px glyph of a
                            // 24 pt font at 22 px).
                            roundToIconSize: false
                            source: glyph
                            isMask: true
                            color: playlistRow.isNow
                                ? playlistRow.nowColor
                                : !playlistRow.showsError ? playlistRow.secondaryTextColor
                                : playlistRow.rowSelected ? root.uiPalette.negativeSelected
                                : root.uiPalette.negative
                            // The amber glyph fades in as the old row's
                            // fades out.
                            opacity: playlistRow.isNow ? playlistRow.amberIn : 1
                        }

                        // The glyph the row had while it was now, fading
                        // out over whatever it shows next.
                        Kirigami.Icon {
                            objectName: "queueRowFadingState"
                            visible: playlistRow.amberLeft > 0
                            anchors.centerIn: parent
                            width: root.uiPalette.scaledSize(12)
                            height: root.uiPalette.scaledSize(12)
                            roundToIconSize: false
                            source: visible ? queueNowFade.target.glyph : ""
                            isMask: true
                            color: playlistRow.nowColor
                            opacity: playlistRow.amberLeft
                        }
                    }

                    // Why an unreadable row is marked (reasonShown), from
                    // its title, below it unless near the view's bottom,
                    // and clear of the playing row: on the 16 px glyph
                    // alone, the pointer had to find it, and the keyboard
                    // never could. Only such a row makes one.
                    Loader {
                        active: playlistRow.unreadable
                        sourceComponent: Components.RowReasonToolTip {
                            objectName: "queueRowReason"
                            row: playlistRow
                            x: titleCell.x
                            rowTop: playlistRow.y - playlistView.contentY
                            viewHeight: playlistView.height
                            room: playlistRow.width - playlistView.rowStatesEnd - titleCell.x
                                - Kirigami.Units.smallSpacing
                            // The rows are alike in height: the playing
                            // row is where its index puts it.
                            coversNow: function(top, bottom) {
                                if (root.uiBridge.playbackState === "Stopped"
                                        || root.uiBridge.playingQueueIndex < 0) {
                                    return false
                                }
                                const nowTop = (root.uiBridge.playingQueueIndex - playlistRow.queueRowIndex)
                                    * playlistRow.height
                                return nowTop < bottom && nowTop + playlistRow.height > top
                            }
                            visible: playlistRow.reasonShown
                            message: playlistRow.unreadableReason
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
                        textFormat: Text.PlainText
                        text: root.playlistOrderText(index)
                        color: root.uiPalette.textTertiaryOn(playlistRow.rowSelected,
                            playlistRowMouseArea.containsMouse)
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
                        textFormat: Text.PlainText
                        text: playlistRow.titleValue
                        readonly property color plainColor: playlistRow.unreadable
                            ? playlistRow.secondaryTextColor : root.uiPalette.text
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
                            ? playlistRow.nowColor
                            : (amber > 0 ? ColorUtils.mixColor(plainColor, playlistRow.nowColor, amber) : plainColor)
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
                        textFormat: Text.PlainText
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
                        textFormat: Text.PlainText
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
                        textFormat: Text.PlainText
                        text: playlistRow.lengthTextValue
                        color: playlistRow.secondaryTextColor
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
                                playlistContextMenu.popupAt(playlistRowMouseArea, mouse.x, mouse.y)
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
                    // The rows Remove acts on: every selected row, when the
                    // menu's row is one of several selected; else its own.
                    readonly property bool actsOnSelection: playlistContextMenu.rowIndex >= 0
                        && root.controller.isIndexSelected(playlistContextMenu.rowIndex)
                        && root.controller.selectedIndices.length > 1

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

                    Components.AppMenuItem {
                        objectName: "queueShowInLibraryItem"
                        text: I18n.i18nc("@action:inmenu", "Show in Library")
                        // The library's tree, as the search result's item.
                        icon.name: "view-list-tree"
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            const path = root.uiBridge.queuePathAt(playlistContextMenu.rowIndex)
                            root.libraryController.showTrackInLibrary(path || "")
                        }
                    }

                    Components.AppMenuItem {
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

                    Components.AppMenuItem {
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
                        // Names how many rows it removes, when it is several.
                        text: playlistContextMenu.actsOnSelection
                            ? I18n.i18ncp("@action:inmenu", "Remove %1 Track from Queue",
                                          "Remove %1 Tracks from Queue",
                                          root.controller.selectedIndices.length)
                            : I18n.i18nc("@action:inmenu", "Remove from Queue")
                        icon.name: "list-remove"
                        keyHint: I18n.i18nc("@info:shortcut key name", "Del")
                        enabled: playlistContextMenu.rowIndex >= 0
                        onTriggered: {
                            if (playlistContextMenu.rowIndex < 0) {
                                return
                            }
                            if (playlistContextMenu.actsOnSelection) {
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
                    textFormat: Text.PlainText
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
                    textFormat: Text.PlainText
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
                    // The desktop style draws the default button from this,
                    // as DialogButtons' primary one: `highlighted` alone
                    // drew it as any other.
                    Accessible.defaultButton: true
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
                visible: root.libraryLoaded && root.uiBridge.queueLength === 0 && !root.firstRun
                anchors.centerIn: parent
                width: Math.max(0, Math.min(parent.width - 6 * Kirigami.Units.largeSpacing,
                                            Math.max(implicitWidth, Kirigami.Units.gridUnit * 20)))
                spacing: Kirigami.Units.smallSpacing

                Text {
                    objectName: "queueEmptyLabel"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
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
                    textFormat: Text.PlainText
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
                    // One pixel up, so the 2 px line straddles the
                    // boundary; inside the view at its ends.
                    queueDropIndicatorY = Math.max(top, Math.min(top + playlistView.height - 2, lineY - 1))
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

            // Where a drop lands: a 2 px line in the focus colour, centred
            // on the boundary between the rows, which ends where the focus
            // ring does.
            Rectangle {
                objectName: "queueDropIndicator"
                x: playlistView.x
                width: Math.max(0, playlistView.width - playlistView.rowStatesEnd)
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
                // On whole pixels, for the frame's edges.
                x: Math.round(queueHeaderRow.x + modelData.header.x - root.columnGap / 2) - resizeHandle.lineX
                width: Kirigami.Units.largeSpacing
                height: parent.height
                // An empty queue's columns have no labels to resize.
                activeFocusOnTab: visible && root.uiBridge.queueLength > 0

                Accessible.role: Accessible.Separator
                readonly property string actionText: I18n.i18nc("@action %1 is a column title",
                    "Resize the %1 column", modelData.label)
                readonly property string keysText: I18n.i18nc("@info",
                    "Left or Right to resize, Home for the default width")
                Accessible.name: resizeHandle.actionText
                Accessible.description: resizeHandle.keysText
                Accessible.focusable: true

                // The ring's inset from the header's top seam and from its
                // hairline, so neither line merges into the ring's edge.
                readonly property real ringInset: 2
                // The divider's x, on a whole pixel, mid-gap.
                readonly property real lineX: Math.floor(width / 2)
                // The ring's half width past the divider: within the gap,
                // 3 px clear of the label after it at least (at 12 px wide
                // it was 2 px off "Artist"). Odd in all, so the divider is
                // its middle pixel.
                readonly property real ringReach: Math.floor(root.columnGap / 2) - 4

                // The divider: 1 px in the tertiary text colour under the
                // pointer (the seam colour, 1.5:1, barely showed), the
                // focus colour while dragged. With keyboard focus it stays
                // drawn inside the ring, so the ring reads as around a
                // column's divider rather than an empty box.
                Rectangle {
                    objectName: "queueColumnHandleLine"
                    x: resizeHandle.lineX
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.topMargin: resizeHandle.activeFocus
                        ? resizeHandle.ringInset + 4 : Kirigami.Units.smallSpacing
                    anchors.bottomMargin: resizeHandle.activeFocus
                        ? 1 + resizeHandle.ringInset + 4 : Kirigami.Units.smallSpacing
                    width: 1
                    visible: resizeHandle.highlighted
                    color: resizeMouseArea.pressed ? root.uiPalette.focusRing : root.uiPalette.textTertiary
                }

                // At rest, over a populated header: a short stroke in the
                // seam colour where the divider is, centred on the labels,
                // so a pointer user can see where to resize (the boundary
                // had nothing to aim at). It gives way to the divider
                // under the pointer, while dragged and with focus.
                Rectangle {
                    objectName: "queueColumnHandleRest"
                    x: resizeHandle.lineX
                    // Centred above the header's 1 px hairline.
                    y: Math.round((resizeHandle.height - 1 - height) / 2)
                    width: 1
                    height: root.uiPalette.scaledSize(10)
                    visible: !resizeHandle.highlighted && root.uiBridge.queueLength > 0
                    color: root.uiPalette.seam
                }

                // With keyboard focus, the app's ring around the divider,
                // within the gap between the columns and clear of the
                // header's top seam and hairline (a 2 px line alone read as
                // a hover).
                Rectangle {
                    objectName: "queueColumnHandleFocus"
                    x: resizeHandle.lineX - resizeHandle.ringReach
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.topMargin: resizeHandle.ringInset
                    // Over the header's hairline.
                    anchors.bottomMargin: 1 + resizeHandle.ringInset
                    width: 2 * resizeHandle.ringReach + 1
                    radius: 3
                    visible: resizeHandle.activeFocus
                    color: "transparent"
                    border.width: 2
                    border.color: root.uiPalette.focusRing
                }

                // Keyboard focus says what the handle is, and its keys.
                ToolTip.visible: resizeHandle.activeFocus && root.keyboardInUse
                ToolTip.delay: Kirigami.Units.toolTipDelay
                ToolTip.text: I18n.i18nc("@info:tooltip what a handle does, then its keys", "%1 · %2",
                    resizeHandle.actionText, resizeHandle.keysText)

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
