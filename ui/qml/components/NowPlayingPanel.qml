// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/PathUtils.js" as PathUtils

// The top of the sidebar: the playing track, on the view colour with no
// card, and a seam at its foot, where the library starts.
//
// - Wide: the cover edge to edge, then the title, "Artist — Album" and
//   "Year · Genre · Track n" under it, inset 12 px, a line each, in a
//   block that with the cover is a whole number of the list's rows high
//   (see wideTextTop), then the seam. The block is one height for every
//   track (otherwise an untagged track's, a tagged one's and one with a
//   long album's were 56, 82 and 108 px, and a track change moved the
//   library by up to 26 px): each line is elided where it does not fit,
//   with the whole text in the tooltip, and a line the track has no text
//   for keeps its row, empty.
// - Narrow (a sidebar under narrowWidth): the cover at the panel's top
//   left corner, as the wide one is (not inset 12 px, at the top of the
//   title's capitals), with four short lines beside it,
//   12 px after it, the cover as high as the block, flush with the seam,
//   where the lines keep a dozen characters beside it, the lines centred
//   on it (centredTextTop), and 96 px otherwise (besideCoverSize), the
//   lines 10 px down; or a 96 px cover with the lines
//   under it when there is no room beside it, then 12 px before the seam.
//   Under it, the title wraps onto a second line rather than be cut off,
//   and under a title on one line the artist, or else the album, and the
//   lines are 4 px apart. Beside the cover in a wide sidebar (the compact
//   layout) the block keeps six lines' room. With a font too large for
//   four lines beside the 96 px cover, as 24 pt, in either sidebar, it is
//   a header row instead, one block at the 12 px inset: the cover as high
//   as two lines of the title, the title centred beside it, and the
//   artist, the album and the details under it on the whole width, all on
//   the cover's left edge (see largeHeader). A sidebar too short for the
//   wide layout to leave the library about six rows, as with a large font
//   or a short window, takes it too, with the lines beside the cover
//   however far past it they run, as its room beside it is wide.
// - Nothing loaded, wide or narrow: one compact row of the list's row
//   height, Ferrous's 16 px mark at the 12 px inset and "Nothing playing"
//   where a library row has its text, both in the secondary colour; the
//   library gets the height (an empty square took 360 px). The first
//   track grows the cover's place from the row to its full size over
//   moveDuration, on the move's curve (moveCurve), uncovering the cover, which lies at its full
//   size in place all along (a cover scaled up from a square in the middle
//   of the band read as a zoom, with empty flanks), fading in as the row
//   goes, by the move's time (see idleOut), the lines and the library
//   moving down with its foot, so nothing jumps. Under the wide cover
//   the move opens the lines' room the faster at first, over the moving
//   seam, the band of the cover growing with it from the first frame
//   (wideTextRoom), each line fading in once it has
//   room, so the title reads in the move's first part, over the library and
//   never under its seam; beside and under the narrow cover each line
//   fades in likewise once it has room over the seam, its title first
//   (in as one late in the move, the header's lines were cut across their
//   letters by the seam as they came in). Going back to nothing
//   loaded, as when the queue is cleared or runs out, is the same move
//   backwards, on its curve and time (rather than the cover and lines
//   going in one frame, leaving a mark alone in the band as it shrank):
//   the track that showed stays, its cover or stand-in and its lines
//   (heldTrack), while it shrinks. Under the wide cover the seam closes
//   the lines' room the faster at first, the cover's foot going up from
//   the first frame too, the lines with it, each line gone as the seam
//   reaches its foot (with the lines out by the time alone, a blank band
//   trailed the cover for some 150 ms); beside and under the narrow cover
//   the cover and the lines stay put as the seam comes up, each line gone
//   as it reaches its foot, then the cover's foot goes up with it; and the
//   row comes back over the move's end. A move turned back goes
//   on from what shows (moveFrom). With animations off, both are at once.
// - A track without a cover keeps the cover's place, wide or narrow, with
//   a stand-in in it (AlbumArtTile): the plot's floor colour with the app
//   icon's harmonic traces (StandInMark), the same for every track and
//   never text (an album title in it said again what the lines under it
//   say, and left the slot blank without one). So the panel changes its
//   geometry only between nothing loaded and a track (a layout of its own
//   for a track without a cover, a mark beside the title and no cover's
//   place, moved the library some 360 to 400 px on a change between the
//   two). Between a track with a cover and one without, the cover and the
//   stand-in cross-fade in place, as two covers do; the lines and the
//   library stay put.
// - Narrow, a line a track has no text for, as for a file without tags,
//   takes no room: the lines under it move up, within the block.
// - Narrow, the block is one height for every track, too, beside the cover
//   and under it (at 24 pt under the cover a title on two lines moved the
//   library 28 px): it keeps room for two lines of the title and one each
//   of the artist, the album and the details (see narrowReservedHeight).
//   Under the cover, a title on one line leaves a line of that room to the
//   artist, or else to the album, to wrap into.
//
// The title is over the rest: DemiBold, and a Kirigami heading's size,
// level 1 under the wide cover and level 2 beside the narrow one, but the
// base size in a large-font header with little room beside its cover
// (headerTitleSmall), one size for every track of a layout; the
// artist is in the text colour, and so is the album where it shares the
// wide layout's line, but a narrow album's own line is in the secondary
// one; the details, one line in one colour, are in the tertiary one.
// Details that do not fit leave out
// what matters least rather than be cut off: the track total, then the
// genre, then the year.
//
// Lines that do not fit are elided, with the whole text in a tooltip.
Item {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer

    // The sidebar width under which the cover sits beside the text: 320 px
    // with the default font, and more with a larger one, as the sidebar is
    // wider then (Main.qml): at 24 pt, 384 px, between the 360 px of a
    // 1000 px window and the 408 px of a 1600 px one.
    readonly property real narrowWidth: Math.max(320, Math.round(160 * root.uiPalette.fontScale))
    readonly property bool narrow: root.width < root.narrowWidth
    // The sidebar's height, and what of it the library under the panel
    // needs: its header and about six rows (SidebarPane). Where the wide
    // layout would leave it less, as with a large font or in a short
    // window, the panel takes the narrow one (at 24 pt in a 1000 px high
    // window the library showed three rows). The wide
    // layout's height is from the font and the width alone, all its lines
    // counted, so the switch does not change what it is decided by, and
    // a track does not switch it.
    property real availableHeight: 0
    property real libraryReserve: 0
    readonly property real wideReservedHeight: root.wideLayoutHeight
    readonly property bool shortHeight: root.availableHeight > 0
        && root.availableHeight - root.wideReservedHeight < root.libraryReserve
    readonly property int textInset: 12
    readonly property int narrowCoverSize: 96
    // The large-font header's cover (see largeHeader): as high as two lines
    // of the title beside it, from the fonts alone, so that it spans them,
    // and never under the narrow 96 px (108 px at 24 pt; 96 px where its
    // title is at the base size, headerTitleSmall).
    readonly property real headerCoverSize: root.headerTitleSmall
        ? Math.max(root.narrowCoverSize, Math.round(headerSmallTitleProbe.implicitHeight))
        : root.headingHeaderCoverSize
    // The header's cover beside a title at the heading's size.
    readonly property real headingHeaderCoverSize: Math.max(root.narrowCoverSize,
                                                            Math.round(narrowTitleProbe.implicitHeight))
    // Whether the large-font header's title is at the base size rather than
    // the heading's, DemiBold, for every track: where the room beside the
    // heading-size header's cover is under a dozen characters of the
    // heading-size title, the panel's own narrowLineMinimum in the title's
    // font, as at 24 pt in the 360 px sidebar of a 1000 px window, but not
    // in the 408 px one of a 1600 px window. From the fonts and the width
    // alone, so every title of a layout is one size (a long title that
    // stepped down a size on its own ranked over the artist by weight alone
    // on that track). The room is the sidebar's, less the two insets: the
    // header's own inset before the cover takes no title's size (counted,
    // it put the 408 px sidebar's titles at the base size too, barely over
    // the artist's).
    readonly property bool headerTitleSmall: root._roomBeside(root.headingHeaderCoverSize)
        < 12 * narrowTitleMetrics.averageCharacterWidth
    // The size of the narrow layout's title: the base size in a large-font
    // header with a small title, and a level 2 heading's otherwise.
    readonly property real narrowTitlePointSize: root.largeHeader && root.headerTitleSmall
        ? root.uiPalette.basePointSize : root.titlePointSize
    // The cover beside the narrow lines (narrowBeside): the block's whole
    // height, flush with its top and with the seam, rather than 96 px with
    // a band of the view under it (25 px at 10 pt); 96 px
    // where the lines would have under narrowLineMinimum beside it, as at
    // 12 and 14 pt in a 260 px sidebar. The block keeps its height.
    readonly property real besideBlockSize: Math.max(root.narrowCoverSize,
                                                     root.narrowTextTop + root.narrowReservedHeight) + root.textInset
    readonly property real besideCoverSize: root._roomBeside(root.besideBlockSize) >= root.narrowLineMinimum
        ? root.besideBlockSize : root.narrowCoverSize
    // The sides of the cover's place where it meets the sidebar's view
    // colour, which the stand-in edges, rather than the panel's top, the
    // window's edge, the sidebar's seam or the panel's (an edge all round
    // doubled them, a purple frame inside the flush narrow tile and a 2 px
    // seam under it): the wide tile's foot alone, the side of the narrow
    // one that faces the lines, with its foot where the block runs on under
    // it, and all four of the large-font header's, inset.
    readonly property var standInEdgeSides: ({
        top: root.coverInset > 0,
        left: root.coverInset > 0,
        right: root.coverInset + root.coverSize < root.width - 0.5,
        bottom: root.coverInset + root.coverSize < root.layoutHeight - root.seamHeight - 0.5
    })
    // The cover of the narrow layout: the large-font header's, the one that
    // fills the block beside the narrow lines, or 96 px.
    readonly property real narrowLayoutCoverSize: root.largeHeader ? root.headerCoverSize
        : root.narrowBeside ? root.besideCoverSize : root.narrowCoverSize
    // The room for lines beside a cover `side` px wide.
    function _roomBeside(side) {
        return root.width - side - 2 * root.textInset
    }
    // The same beside the large-font header's cover, which is at the inset.
    function _roomBesideHeader(side) {
        return root.width - root.textInset - side - root.besideGap - root.headerTitleEnd
    }
    // Where the cover is in from the panel's corner: at the inset in the
    // large-font header, on its lines' left edge, and at the corner
    // elsewhere.
    readonly property int coverInset: root.largeHeader ? root.textInset : 0

    readonly property bool hasTrack: root._bridgeHasTrack()
    // Whether the bridge tells of a track, as it does now (see
    // _bridgeTrackKey).
    function _bridgeHasTrack() {
        const hasMetadata = root._trimmed(root.uiBridge.currentTrackTitle).length > 0
            || root._trimmed(root.uiBridge.currentTrackArtist).length > 0
            || root._trimmed(root.uiBridge.currentTrackAlbum).length > 0
        const hasActivePath = (root.uiBridge.playbackState || "") !== "Stopped"
            && root._trimmed(root.uiBridge.currentTrackPath).length > 0
        return hasMetadata || hasActivePath
    }
    // Nothing loaded: the compact row.
    readonly property bool idle: !root.hasTrack
    // Whether the sidebar has room for the wide layout. "Nothing playing"
    // keeps the layout of the track before, which it shrinks from.
    readonly property bool wideRoom: !root.narrow && !root.shortHeight
    readonly property bool hasCover: root.coverPath.length > 0
    readonly property string layoutMode: root.wideRoom ? "wide" : "narrow"

    // The time of a move that carries the library under the panel: the
    // track's layout growing from the compact row or shrinking into it, on
    // moveCurve. Under the wide
    // cover the library moves 350 to 420 px: over longDuration, OutCubic,
    // its first frames jumped some 100 px, four of its rows, and read as a
    // cross-fade rather than a slide; over veryLongDuration, OutCubic, it
    // still started at some 3,100 px/s; over wideMoveDuration, InOutQuad,
    // it peaked at some 1,850 px/s, mid-way, after ~100 ms in which it
    // hardly moved, a pause and then a sweep.
    // The narrow cover's 100 px or so take longDuration. None with
    // animations off.
    readonly property int wideMoveDuration: Math.round(Kirigami.Units.veryLongDuration * 9 / 8)
    readonly property int moveDuration: Kirigami.Units.longDuration <= 0 ? 0
        : (root.wideRoom ? root.wideMoveDuration : Kirigami.Units.longDuration)
    // The move's curve: InOutQuad with a share, moveLead, of OutQuad, so it
    // sets off at once, at half the mean speed (~460 px/s under the wide
    // cover, some 8 px in the first frame), peaks mid-way at 1.75 times it
    // (~1,600 px/s), and eases into place: one movement from the first
    // frame, both ways, rather than a hold and a sweep. Two quadratic
    // halves, exact as Bézier segments with even steps in time, so the
    // animation's curve and _movePresence are one.
    readonly property real moveLead: 0.25
    readonly property real moveMiddle: (2 + root.moveLead) / 4
    readonly property var moveCurve: [
        1 / 6, root.moveLead / 3,
        1 / 3, (2 * root.moveLead + (2 - 3 * root.moveLead) / 4) / 3,
        0.5, root.moveMiddle,
        2 / 3, (root.moveMiddle + 2) / 3,
        5 / 6, 1,
        1, 1]
    // In the narrow layouts a line has room from the presence where the
    // seam passes its foot, `foot` once in place: until then the foot moves
    // up from under the compact row, or under the cover moves down with
    // it. Never before the row is gone. (In as one from 60 % of the way,
    // the header's lines were cut across their letters by the seam.)
    function _narrowRoomPresence(foot) {
        const from = root.moveFrom
        const band = from.band >= 0 ? Math.min(root.coverSize, from.band)
                                    : Math.min(root.coverSize, root.idleRowHeight)
        const moves = (root.narrowStacked ? band - root.coverSize : from.shift * root.idleRowHeight)
            / Math.max(1e-6, 1 - from.presence)
        const met = root.moveTravel + moves > 0
            ? (foot - root.idleRowHeight + moves) / (root.moveTravel + moves) : 1
        return Math.min(1, Math.max(met, root._movePresence(
            root.idleOut[1] * Kirigami.Units.shortDuration / Math.max(1, root.moveDuration))))
    }
    // The height the move grows to, or shrinks from.
    readonly property real movedHeight: root.idle ? root.heldLayoutHeight : root.layoutHeight
    // The move's travel: the library's under the panel.
    readonly property real moveTravel: Math.max(0, root.movedHeight - root.restHeight)
    // Where the panel's seam is now, from its top.
    readonly property real moveSeam: root.idleRowHeight + root.moveTravel * root.presence
    // Where the move under way set out from, which followIdle takes as it
    // starts or turns back, so that nothing jumps as it does: `closing`,
    // whether it goes back to nothing loaded; `presence`; the cover's
    // `band`, or -1 for the compact row's; the wide lines' `room`; how far
    // the lines are still down by the row (`shift`, of its height); and
    // each line's opacity, by its name (`levels`).
    readonly property var moveFromRest: ({ closing: false, presence: 0, band: -1, room: 0, shift: 1, levels: {} })
    property var moveFrom: root.moveFromRest
    // How much of the move from `moveFrom` to whole is done, opening.
    readonly property real openProgress: root.moveFrom.presence >= 1 ? 1
        : Math.min(1, Math.max(0, (root.presence - root.moveFrom.presence) / (1 - root.moveFrom.presence)))
    // How far the lines are down by the compact row, of its height: up
    // into place as the move opens, and where they are as it closes, so
    // that the seam takes them in rather than they slide under it.
    readonly property real lineShift: root.presence >= 1 ? 0
        : root.moveFrom.closing ? root.moveFrom.shift
        : root.moveFrom.shift * (1 - root.openProgress)
    // Under the wide cover the move gives the lines their room the faster
    // at first, over the moving seam (the title waited behind the cover's
    // reveal: at +276 ms of 450 the band under a growing piece of the cover
    // was still empty), but not all of it: wideRoomShare of the seam's
    // travel at the start, so the band of the cover grows from the first
    // frame too (with all of it, the band held at the row's height for
    // some 100 ms, a strip, and then swept). The room's share eases out to
    // nothing as it is whole, by wideRoomSpan of the way, some 190 ms of
    // the 450, so the band's growth takes the seam's over with no jolt.
    // Closing, the cover's foot goes up with the seam from the first frame,
    // the lines with it, and the room closes on a smooth step, at most
    // that share of the seam's travel, over wideCloseSpan (closed the
    // faster at first, as it opens, its empty end, the title gone, trailed
    // the cover for some 100 ms).
    readonly property real wideRoomShare: 0.6
    readonly property real wideRoomWhole: Math.max(0, root.movedHeight - root.seamHeight - root.coverSize)
    readonly property real wideRoomSpan: root.moveTravel > 0
        ? Math.min(1 - root.moveFrom.presence,
                   2 * Math.max(0, root.wideRoomWhole - root.moveFrom.room)
                       / (root.wideRoomShare * root.moveTravel))
        : 1
    readonly property real wideCloseSpan: root.moveTravel > 0
        ? Math.min(root.moveFrom.presence,
                   1.5 * Math.max(0, root.moveFrom.room) / (root.wideRoomShare * root.moveTravel))
        : root.moveFrom.presence
    readonly property real wideTextRoom: {
        if (root.presence >= 1)
            return root.wideRoomWhole
        if (root.moveFrom.closing) {
            if (root.wideCloseSpan <= 0)
                return 0
            const closed = Math.min(1, Math.max(0, root.moveFrom.presence - root.presence) / root.wideCloseSpan)
            return root.moveFrom.room * (1 - closed * closed * (3 - 2 * closed))
        }
        if (root.wideRoomSpan <= 0)
            return root.wideRoomWhole
        const rest = 1 - Math.min(1, Math.max(0, root.presence - root.moveFrom.presence) / root.wideRoomSpan)
        return root.wideRoomWhole - (root.wideRoomWhole - root.moveFrom.room) * rest * rest
    }
    // Where the seam is as the compact row starts to come back, which
    // every line is gone by, so that no two texts show at once.
    readonly property real rowBackPresence: root._movePresence(
        root.idleOut[1] * Kirigami.Units.shortDuration / Math.max(1, root.moveDuration))
    readonly property real rowBackSeam: root.idleRowHeight + root.moveTravel * root.rowBackPresence
    // The opacity of the line `name`, whose text's foot is `foot` from the
    // panel's top and `placedFoot` once in place, and which under the wide
    // cover needs `room` under that, -1 in the narrow layouts. Opening, it
    // fades in over shortDuration, OutCubic, from when there is room for
    // it, never clipped by the seam nor over the library. Closing, it fades
    // out as the seam comes up to its foot: under the wide cover, which the
    // lines go up with, as their room closes to what the line needs. Read
    // from the move's progress,
    // so a move that turns back, or none, as with animations off, takes it
    // with it.
    function _lineLevel(name, foot, placedFoot, room) {
        if (root.presence >= 1)
            return 1
        if (root.presence <= 0 || root.moveDuration <= 0)
            return 0
        const from = root.moveFrom.levels[name] || 0
        if (root.moveFrom.closing) {
            if (room >= 0) {
                // Where the room is down to what the line needs: the smooth
                // step's inverse (wideTextRoom).
                const roomFrom = root.moveFrom.room
                const reached = roomFrom > room
                    ? root.moveFrom.presence - root.wideCloseSpan
                        * (0.5 - Math.sin(Math.asin(2 * room / roomFrom - 1) / 3))
                    : root.moveFrom.presence
                const goneAt = Math.max(reached, root.rowBackPresence)
                const awayFor = root.moveFrom.presence - goneAt
                return awayFor > 0 ? from * Math.min(1, Math.max(0, root.presence - goneAt) / awayFor) : 0
            }
            const gone = Math.max(foot, root.rowBackSeam)
            const away = root.idleRowHeight + root.moveTravel * root.moveFrom.presence - gone
            return away > 0 ? from * Math.min(1, Math.max(0, root.moveSeam - gone) / away) : 0
        }
        let start = 0
        if (room >= 0) {
            const opening = root.wideRoomWhole - root.moveFrom.room
            const share = opening > 0 ? Math.min(1, Math.max(0, room - root.moveFrom.room) / opening) : 0
            start = root.moveFrom.presence + root.wideRoomSpan * (1 - Math.sqrt(1 - share))
        } else {
            start = root._narrowRoomPresence(placedFoot)
        }
        const startTime = root._moveTime(Math.max(start, root.moveFrom.presence)) * root.moveDuration
        const time = Math.max(1, Math.min(Kirigami.Units.shortDuration, root.moveDuration - startTime))
        return from + (1 - from) * root._eased((root.moveElapsed - startTime) / time)
    }
    // The compact row of nothing loaded and the cover hand over by the
    // move's time, not by how far it has come, so they take the same
    // milliseconds over any move: the row goes over idleOut, linear, the
    // cover comes in over coverIn, OutCubic, whole for the rest; they
    // overlap only where each is near half, so the band never shows
    // neither, nor "Nothing playing" across the art for long (by the
    // move's progress, on InOutQuad's slow start the row was gone by 12 %
    // of the way and the cover still faint, so the band was empty for some
    // 60 to 140 ms). As fractions of shortDuration, [start, end]: the row from
    // 20 to 50 ms, the cover from 12 to 112 ms, with Kirigami's own. Going
    // back to nothing loaded runs the same backwards.
    readonly property var idleOut: [0.2, 0.5]
    readonly property var coverIn: [0.12, 1.12]
    // How long the move has run at `presence`, in ms.
    readonly property real moveElapsed: root._moveTime(root.presence) * root.moveDuration
    function _handoverProgress(span) {
        const short = Math.max(1, Kirigami.Units.shortDuration)
        return Math.min(1, Math.max(0, (root.moveElapsed - span[0] * short) / ((span[1] - span[0]) * short)))
    }
    readonly property bool moveEnded: root.presence >= 1 || root.moveDuration <= 0
    readonly property real coverOpacity: root.presence <= 0 ? 0
        : root.moveEnded ? 1 : root._eased(root._handoverProgress(root.coverIn))
    // How far into the move, from 0 to 1, it is `presence` of the way:
    // moveCurve's inverse.
    function _moveTime(presence) {
        const p = Math.min(1, Math.max(0, presence))
        const lead = root.moveLead
        if (p >= root.moveMiddle)
            return 1 - Math.sqrt((1 - p) / (2 - lead))
        const square = 2 - 3 * lead
        return square > 0 ? (Math.sqrt(lead * lead + square * p) - lead) / square : p / (2 * lead)
    }
    // How far the move has come `time` of the way through it: moveCurve.
    function _movePresence(time) {
        const t = Math.min(1, Math.max(0, time))
        const lead = root.moveLead
        return t < 0.5 ? 2 * lead * t + (2 - 3 * lead) * t * t : 1 - (2 - lead) * (1 - t) * (1 - t)
    }
    readonly property string idleText: I18n.i18nc("@info", "Nothing playing")

    // The compact row of nothing loaded: a list row's height, with the
    // library's glyph size and gap, both following the font, so its text
    // starts where a library row's does (a fixed 6 px gap left the label
    // pressed against the 32 px glyph at 24 pt).
    readonly property real idleRowHeight: root.uiPalette.rowHeight
    readonly property int idleGlyphSize: root.uiPalette.toolIconSize
    // Ferrous's mark drawn for the idle glyph's size, by one rule for every
    // drawing of it: 20 px and under, the glide and its 3rd harmonic, as
    // the library's 20 px tile draws them (three 1.5 px strokes read as a
    // slanted hatch at 16 px); 24 to 108 px, three traces; the 360 px
    // stand-in, four.
    readonly property url idleGlyphSource: root.idleGlyphSize <= 20
        ? Qt.resolvedUrl("../assets/ferrous-mark-small-symbolic.svg")
        : Qt.resolvedUrl("../assets/ferrous-mark-symbolic.svg")
    readonly property int idleGlyphGap: root.uiPalette.scaledSize(6)
    // The panel's height with nothing loaded: the row and the seam.
    readonly property real restHeight: root.idleRowHeight + root.seamHeight
    // The side of the cover's square, at the panel's top left: the
    // sidebar's width in the wide layout, and in the narrow one
    // narrowLayoutCoverSize, whether the track has a cover or its stand-in.
    readonly property real coverSize: root.wideRoom ? root.width : root.narrowLayoutCoverSize
    // How far the playing track's layout has grown from the compact row:
    // 0 with nothing loaded, 1 with a track, between while it grows or
    // shrinks. The cover's place, the panel's height and, by the move's
    // time, the cover's opacity and the compact row's follow it (see
    // idleOut).
    property real presence: 0
    readonly property real idleRowOpacity: root.presence <= 0 ? 1
        : root.moveEnded ? 0 : 1 - root._handoverProgress(root.idleOut)
    // The full layout's height as it showed when the track went: the
    // height the panel shrinks from, which its lines, gone with the
    // track, no longer give.
    property real heldLayoutHeight: 0
    // The track as it last showed, {coverPath, lines} (see followTrack):
    // what the panel shows while it shrinks into the compact row, as the
    // bridge tells nothing of the track any more, before or after the
    // panel is idle. Kept while a track plays, so that it is the
    // one that showed whichever the bridge clears first; gone once the
    // panel has shrunk.
    property var heldTrack: null
    // Whether the panel shows the track that went, as it shrinks.
    readonly property bool holdsTrack: root.idle && root.presence > 0 && root.heldTrack !== null
    // The lines and the cover the panel shows: the playing track's, or the
    // one's that went while the panel shrinks.
    readonly property var shownLines: root.holdsTrack ? root.heldTrack.lines : root.trackLines
    readonly property string shownCoverPath: root.holdsTrack ? root.heldTrack.coverPath : root.coverPath
    onPresenceChanged: {
        if (root.idle && root.presence <= 0)
            root.heldTrack = null
    }
    readonly property NumberAnimation presenceAnimation: NumberAnimation {
        objectName: "nowPlayingPresenceAnimation"
        target: root
        property: "presence"
        easing.type: Easing.BezierSpline
        easing.bezierCurve: root.moveCurve
        // The curve's last step can fall a rounding error short of its end,
        // which would leave the layout a hair from whole.
        onFinished: root.presence = root.presenceAnimation.to
    }
    readonly property bool growing: root.presence > 0 && root.presence < 1
    // `size` on whole pixels while the layout grows or shrinks, so the
    // library under the panel and the lines move by whole pixels, crisp.
    function _whileGrowing(size) {
        return root.growing ? Math.round(size) : size
    }

    function followIdle() {
        const to = root.idle ? 0 : 1
        if (root.idle && root.presence > 0) {
            // The panel's height is still what the last frame laid out:
            // the sidebar lays it out again only before the next.
            root.heldLayoutHeight = root.restHeight
                + Math.max(0, root.height - root.restHeight) / root.presence
        }
        root.moveFrom = root._moveFromHere(root.idle)
        root.presenceAnimation.stop()
        if (root.moveDuration <= 0 || root.presence === to) {
            root.presence = to
            return
        }
        root.presenceAnimation.from = root.presence
        root.presenceAnimation.to = to
        root.presenceAnimation.duration = root.moveDuration
        root.presenceAnimation.start()
    }
    onIdleChanged: root.followIdle()
    // The move that starts now, from what shows (see moveFrom).
    function _moveFromHere(closing) {
        if (root.presence <= 0)
            return root.moveFromRest
        const levels = {}
        for (const line of [wideTitle, wideArtistAlbum, wideDetails,
                            narrowTitle, narrowArtist, narrowAlbum, narrowDetails])
            levels[line.name] = line.opacity
        return { closing: closing, presence: root.presence, band: root.coverBand, room: root.wideTextRoom,
                 shift: root.lineShift, levels: levels }
    }

    readonly property string titleText: {
        const title = root._trimmed(root.uiBridge.currentTrackTitle)
        return title.length > 0
            ? title
            : FormatUtils.basenameFromPath(root._trimmed(root.uiBridge.currentTrackPath))
    }
    readonly property string artistText: root._trimmed(root.uiBridge.currentTrackArtist)
    readonly property string albumText: root._trimmed(root.uiBridge.currentTrackAlbum)
    readonly property string genreText: root._trimmed(root.uiBridge.currentTrackGenre)
    readonly property string yearText: {
        const year = root.uiBridge.currentTrackYear
        return year !== undefined && year !== null ? String(year).trim() : ""
    }
    // The total only when the backend knows it surely.
    readonly property string trackNumberText: {
        const number = root.uiBridge.currentTrackNumber
        const total = root.uiBridge.currentTrackTotal
        if (!(number > 0))
            return ""
        return total >= number
            ? I18n.i18nc("@info the playing track's number and the tracks on its album",
                         "Track %1 of %2", number, total)
            : I18n.i18nc("@info the playing track's number on its album", "Track %1", number)
    }
    // The track's place on its album without the word "Track", "2 of 8",
    // after the genre on a line with no room for the whole (see
    // _fittingDetails); "" without a total the backend is sure of.
    readonly property string albumPositionText: {
        const number = root.uiBridge.currentTrackNumber
        const total = root.uiBridge.currentTrackTotal
        return number > 0 && total >= number
            ? I18n.i18nc("@info the playing track's number and the tracks on its album, after its genre",
                         "%1 of %2", number, total)
            : ""
    }
    // The same without the total, for a line with no room for it.
    readonly property string trackNumberShortText: {
        const number = root.uiBridge.currentTrackNumber
        return number > 0
            ? I18n.i18nc("@info the playing track's number on its album", "Track %1", number) : ""
    }
    // The separator between the fields of a line, for the lines that have
    // more than one.
    readonly property var lineSeparators: ({ artistAlbum: " — ", details: " · ", narrowDetails: " · " })

    // Missing values are left out rather than shown as dashes.
    readonly property string artistAlbumText: root._joined([root.artistText, root.albumText],
                                                          root.lineSeparators.artistAlbum)
    readonly property string detailsText: root._joined(
        [root.yearText, root.genreText, root.trackNumberText], root.lineSeparators.details)
    readonly property string narrowAlbumText: root.albumText.length > 0 && root.yearText.length > 0
        ? I18n.i18nc("@info album and its year", "%1 (%2)", root.albumText, root.yearText)
        : (root.albumText.length > 0 ? root.albumText : root.yearText)
    // "Album (Year)" as the album and its year, [album, " (Year)"], or
    // [text, ""] for a text without both: the album line draws the year
    // whole after the album, which is cut off before it where the two do
    // not fit, "A Remarkabl… (2024)" (at 24 pt the year was lost to the
    // ellipsis). The parts are read from the line's own
    // format, so a translation that puts the year first has none.
    readonly property var albumYearFormat: {
        const format = I18n.i18nc("@info album and its year", "%1 (%2)", "\u0001", "\u0002")
        const year = format.indexOf("\u0002")
        return format.indexOf("\u0001") === 0 && year > 1
            ? { between: format.slice(1, year), after: format.slice(year + 1) } : null
    }
    function _albumAndYear(text) {
        const format = root.albumYearFormat
        if (format === null || !text.endsWith(format.after))
            return [text, ""]
        const end = text.length - format.after.length
        const at = text.lastIndexOf(format.between, end - 1)
        return at > 0 && /^[0-9]+$/.test(text.slice(at + format.between.length, end))
            ? [text.slice(0, at), text.slice(at)] : [text, ""]
    }
    // The narrow and compact layouts' details: one grammar for every track,
    // the genre and the track's place, "Classical · 2 of 8", or "Track 2
    // of 8" with no genre (dropping the word "Track" by width alone, the
    // line read "Jazz · Track 1 of 8" on one track and "Classical · 1 of 4"
    // on the next). The wide layout keeps "Track 2 of 8" after the genre.
    readonly property bool narrowPositionAfterGenre: root.genreText.length > 0 && root.albumPositionText.length > 0
    readonly property string narrowDetailsText: root._joined(
        [root.genreText, root.narrowPositionAfterGenre ? root.albumPositionText : root.trackNumberText],
        root.lineSeparators.narrowDetails)
    // A file without tags, with its file name for a title and no artist,
    // album or details, says where it is instead: the name of its folder,
    // in the artist's row, in textSecondary, as a fact of the file rather
    // than a tag (at 24 pt "track07" stood alone beside the cover over
    // some 140 px of empty rows). Never for a track with any
    // tag the lines show, as an album or a track number alone.
    // Its title is the file's own name when it has no title tag: the
    // queue row's title or the bridge's fallback, as the app never leaves
    // it empty.
    readonly property bool untagged: (root._trimmed(root.uiBridge.currentTrackTitle).length === 0
            || PathUtils.isFileNameTitle(root.uiBridge.currentTrackTitle, root.uiBridge.currentTrackPath))
        && root.artistText.length === 0 && root.albumText.length === 0 && root.detailsText.length === 0
    readonly property string folderText: root.untagged
        ? PathUtils.containingFolderName(root._trimmed(root.uiBridge.currentTrackPath)) : ""
    // What a file without tags is, for its details line: its container
    // and its length, "FLAC · 0:40", or the one of them the bridge knows,
    // in the details' style (at 24 pt the tag-less card stood some 100 px
    // empty under the folder). Only for such a file.
    readonly property string fileFactsText: root.untagged
        ? root._joined([root._trimmed(root.uiBridge.currentTrackFormatLabel),
                        root.uiBridge.durationSeconds > 0
                            ? FormatUtils.formatClockTime(root.uiBridge.durationSeconds) : ""],
                       root.lineSeparators.details)
        : ""
    // The mark that starts a line's text where it is the folder's rather
    // than a tag's: the line draws it in textSecondary, without the mark
    // (LineText), and it fades whole, as nothing of it stays in place.
    readonly property string folderMark: "\u0001"
    function _isFolderLine(text) {
        return text.startsWith(root.folderMark)
    }
    // The mark that starts the narrow details where they are a file's
    // facts (fileFactsText): a word joiner, which has no width, so what
    // stays still of them is measured as drawn (#537).
    readonly property string factsMark: "\u2060"
    function _isFactsLine(text) {
        return text.startsWith(root.factsMark)
    }
    function _folderLine(tagText) {
        return root.folderText.length > 0 ? root.folderMark + root.folderText : tagText
    }
    // The details as the lines show them: all of them where the line has
    // room, and otherwise without what matters least, rather than cut off
    // (at 24 pt, "Classical · Track 2 o…"): first the word "Track" after
    // the genre ("Classical · 2 of 8"), then the genre, keeping the track's
    // place on its album ("Track 2 of 8"), then the album's track total,
    // then the year. The narrow sidebar dropped the total first, so
    // "Classical · Track 1" lost the album position the line had room for.
    // The whole of them is in the tooltip.
    function _fittingDetails(year, separator, width, positionAfterGenre) {
        const candidates = [
            positionAfterGenre ? [year, root.genreText, root.albumPositionText]
                               : [year, root.genreText, root.trackNumberText],
            root.genreText.length > 0 && root.albumPositionText.length > 0
                ? [year, root.genreText, root.albumPositionText] : [],
            [year, root.trackNumberText],
            [year, root.trackNumberShortText],
            [root.trackNumberShortText]
        ]
        let last = ""
        for (const fields of candidates) {
            const text = root._joined(fields, separator)
            if (text.length === 0)
                continue
            if (root._tabularWidth(text) <= width)
                return text
            last = text
        }
        // Nothing fits: the shortest, cut off; or a line without a track
        // number, as it is.
        return last.length > 0 ? last : root._joined([year, root.genreText], separator)
    }
    // The width of `text` in the details' font with tabular figures, in
    // which every digit is as wide as the widest.
    function _tabularWidth(text) {
        return Math.ceil(narrowLineMetrics.advanceWidth(text.replace(/[0-9]/g, root.widestDigit)))
    }
    readonly property string widestDigit: {
        let widest = "0"
        for (let digit = 1; digit <= 9; ++digit) {
            if (narrowLineMetrics.advanceWidth(String(digit)) > narrowLineMetrics.advanceWidth(widest))
                widest = String(digit)
        }
        return widest
    }
    readonly property string shownDetailsText: root._fittingDetails(
        root.yearText, root.lineSeparators.details, Math.max(0, root.width - 2 * root.textInset), false)
    readonly property string shownNarrowDetailsText: root._fittingDetails(
        "", root.lineSeparators.narrowDetails, root.largeHeader ? root.footLineWidth : narrowColumn.width,
        root.narrowPositionAfterGenre)

    // The lines the panel shows, wide and narrow.
    readonly property var trackLines: ({
        title: root.titleText,
        artistAlbum: root._folderLine(root.artistAlbumText),
        details: root.untagged ? root.fileFactsText : root.shownDetailsText,
        artist: root._folderLine(root.artistText),
        // A file's facts in the album's row, which it has nothing for, right
        // under its folder in the artist's, so the large-font header keeps
        // its spare row at its foot, and the lines of a tagged track and an
        // untagged file are in the same rows.
        narrowAlbum: root.fileFactsText.length > 0 ? root.factsMark + root.fileFactsText : root.narrowAlbumText,
        narrowDetails: root.untagged ? "" : root.shownNarrowDetailsText
    })

    // A track change fades, line by line: a line that differs for the next
    // track changes in place, the one before going out before the next
    // track's comes in (see linesOutTime), so the two texts never show
    // together; where only a word or a number of it changes,
    // its fields, and the words of a field in which only a number changes,
    // that are the same for both and do not move stay still (#537), and
    // otherwise it fades whole (see `_fadesWhole`); a line that is the same
    // stays still. The next
    // track of the same album fades over shortDuration; a track of another
    // album, or with another cover, fades the cover and the lines together
    // over longDuration, the cover cross-fading over all of it. A line
    // that comes to differ while the track change fades (the metadata
    // comes after the path) joins the fade from full opacity, and the
    // lines already fading go on from theirs. A cover that changes later
    // for the same track fades too: during a track change it joins it, and
    // every layer goes on from the opacity it has onto the long curve;
    // after one, the cover fades alone and the lines change in place.
    // Another track during a fade starts its fade from what shows: every
    // layer still fading goes on from its opacity, and what the track
    // before had faded in goes out from where it got to (#463). What shows
    // is always the playing track's; what the panel showed before only
    // fades out under or over it. A track without a cover has its stand-in
    // for one (see the top), which fades as a cover does: over another
    // cover, and over the stand-in of another album, with its own texts.
    // Nothing fades into or out of "Nothing
    // playing": the first track after it settles its cover and lines
    // together, whatever order the bridge tells them in. With animations
    // off every change is instant.
    readonly property string trackKey: root._bridgeTrackKey()
    // The playing track's key as the bridge tells it now, which followTrack
    // goes by rather than `trackKey`: a change of the path reaches the
    // bindings that follow it one by one, so `trackLines` can change
    // before `trackKey` does, and an untagged file's lines, which come from
    // its path, then read as the same track's and switched in place.
    function _bridgeTrackKey() {
        return root._bridgeHasTrack() ? root._trimmed(root.uiBridge.currentTrackPath) : ""
    }
    readonly property string coverPath: root.uiBridge.currentTrackCoverPath || ""
    // The playing track as last seen.
    property var shown: null
    // Whether the track has just left "Nothing playing" and has had no
    // cover since: the first one it gets is part of that change.
    property bool settling: false
    // Whether a cover came while "Nothing playing" showed: the next
    // track's, told before its path.
    property bool coverCameWhileEmpty: false
    // What fades out, or null: `lines`, each line that fades by its key
    // in `trackLines` (see `_fadingLine`); `covers`, the covers before,
    // oldest first, each {coverPath, from, title}, fading out from
    // `from`, the last a stand-in's title;
    // whether the fade is a track change (`track`), which lines that come
    // to differ join; and whether it runs over longDuration (`long`): for
    // another cover or another album, even when the two look alike.
    property var outgoing: null
    // How far the fade has come in its time: 0 when it starts, 1 once the
    // playing track alone shows. Every layer eases on it (see linesOutTime).
    property real handover: 1
    readonly property NumberAnimation handoverAnimation: NumberAnimation {
        objectName: "nowPlayingHandoverAnimation"
        target: root
        property: "handover"
        from: 0
        to: 1
        easing.type: Easing.Linear
        onFinished: root.outgoing = null
    }

    // The OutCubic curve of the motion.
    function _eased(progress) {
        const rest = 1 - Math.min(1, Math.max(0, progress))
        return 1 - rest * rest * rest
    }
    // A line that changes shows one text in every frame and keeps it
    // readable: the text before goes out first, InQuad, so it holds its
    // ink at the start, over linesOutTime, 30 % of shortDuration, and the
    // playing track's comes in as it ends, OutCubic, over the rest, so the
    // line is under half of a text's ink for ~23 ms of shortDuration's 100,
    // under two frames. The lines' change takes shortDuration in the long
    // fade too, while the cover cross-fades over all of it. (Out over 40 %
    // with OutCubic left each line under half for ~45 ms, a blink; out and
    // in over the same time showed two titles at once, a ghost.) Each line
    // keeps its place on the two curves (`outAt`, `inAt`, see _fadingLine)
    // when the fade starts again, as it does when the tags come after the
    // path, so the line goes on from where it is: started again from the
    // top, the text going out waited out a second linesOutTime from what
    // was left of it, and the line stayed under half for ~75 ms.
    readonly property real linesOutTime: 3 * Kirigami.Units.shortDuration / 10
    readonly property real linesInTime: Kirigami.Units.shortDuration - root.linesOutTime
    // How long the fade has run, in ms.
    readonly property real handoverTime: root.handover * root.handoverAnimation.duration
    // The InQuad curve of a text going out: how much of its opacity it
    // keeps at `position`, and the position at which it keeps `level`.
    function _outCurve(position) {
        const at = Math.min(1, Math.max(0, position))
        return 1 - at * at
    }
    function _outPosition(level) {
        return Math.sqrt(1 - Math.min(1, Math.max(0, level)))
    }
    // The position on the OutCubic curve at which a text has come in to
    // `level`.
    function _inPosition(level) {
        return 1 - Math.cbrt(1 - Math.min(1, Math.max(0, level)))
    }
    // How much of their opacity the layers before of a fading `line` keep:
    // the one that shows most follows the curve on from where it was when
    // the fade started, the others under it alike.
    function _lineOutLevel(line) {
        if (line.outAt >= 1 || root.linesOutTime <= 0)
            return 0
        return root._outCurve(line.outAt + root.handoverTime / root.linesOutTime)
            / root._outCurve(line.outAt)
    }
    // How far the playing track's text of a fading `line` has come in: on
    // from where it was when the fade started, once the line's text before
    // has gone.
    function _lineInOpacity(line) {
        if (root.linesInTime <= 0)
            return 1
        const start = root.linesOutTime * (1 - line.outAt)
        return root._eased(line.inAt + Math.max(0, root.handoverTime - start) / root.linesInTime)
    }
    // How much of their opacity the covers before keep at `at` of the fade,
    // going out over all of it.
    function _coversOutLevel(at) {
        return 1 - root._eased(at)
    }
    readonly property real coversOutLevel: root._coversOutLevel(root.handover)
    // The most layers a line, or the cover, fades out at once: those of
    // the two tracks before the playing one. Past that, a change adds no
    // layer, and none is dropped while it shows: the playing track's own
    // layer takes the new text, or cover, at the opacity it has, as late
    // metadata does, so no opacity jumps. Only a third track change within
    // one fade comes to that. A layer that has faded out is dropped.
    readonly property int maxOutgoingLayers: 2

    // A line that fades: the words of it that stay still (`still`, see
    // `_stillRuns`), set when the line starts to fade and only ever cut
    // down, the same in every text the line shows and in the same place;
    // the lines before that fade out under the playing track's, oldest
    // first (`layers`, see `_layer`); the playing track's text but where
    // the line stays still, which fades in as one, from 1 - `incomingFrom`;
    // and whether that text is the playing track's own (`own`), or still
    // the one of the track before, which goes out as a layer of its own once
    // the playing track's comes. The parts of a line before are cut where
    // its own text has gaps, which the playing track's text need not have
    // there, so that is cut only at the edges of what stays still, in gaps
    // all share. Where the line starts on the curves: the text before that
    // shows most at the position on the out curve where it has the
    // opacity it starts from (`outAt`, 1 with none), and the playing
    // track's at the position on the in curve it has come in to (`inAt`).
    function _fadingLine(still, layers, incomingFrom, own) {
        const outFrom = layers.length > 0 ? Math.max(...layers.map(layer => root._layerFrom(layer))) : 0
        return { still: still, layers: layers, incomingFrom: incomingFrom, own: own,
                 outAt: root._outPosition(outFrom), inAt: root._inPosition(1 - incomingFrom) }
    }

    // A line before as it fades out (`text`), in parts that fade out from
    // the opacity each had when this fade started (`parts`, [{at, from}],
    // in order, the first at 0). A part runs from the middle of the gap
    // after the first `at` characters of `text` (from the line's start for
    // 0) to where the next one starts. A part at 0 draws nothing. Where the
    // line stays still, the playing track's text draws instead.
    function _layer(text, parts) {
        return { text: text, parts: parts }
    }

    // The line before of `key` as it fades out, or null.
    function _outgoingLine(key) {
        return root.outgoing !== null ? (root.outgoing.lines[key] || null) : null
    }

    // The opacity of the part of the lines before of `key` that shows most,
    // on the fade's curve.
    function _outgoingLineOpacity(key) {
        const line = root._outgoingLine(key)
        if (line === null || line.layers.length === 0)
            return 0
        return Math.max(...line.layers.map(layer => root._layerFrom(layer)))
            * root._lineOutLevel(line)
    }

    // How much of the playing track's text of `key` has still to come in.
    function _incomingLineRest(key) {
        const line = root._outgoingLine(key)
        return line === null ? 0 : 1 - root._lineInOpacity(line)
    }

    function _layerFrom(layer) {
        return Math.max(...layer.parts.map(part => part.from))
    }

    // The line of `key` while it still fades, out or in, or null.
    function _shownLine(key) {
        return root._outgoingLineOpacity(key) > 0 || root._incomingLineRest(key) > 0
            ? root._outgoingLine(key) : null
    }

    // Whether `text` has a right-to-left character: its fields are then
    // not laid out from the left, so none of it stays still.
    function _hasRightToLeft(text) {
        return /[֐-ࣿיִ-﷿ﹰ-﻿‏‫‮⁧]|[\uD802\uD803\uD83A\uD83B][\uDC00-\uDFFF]/
            .test(text)
    }

    // The lines whose numbers are tabular figures, where a digit that
    // changes moves nothing after it.
    readonly property var tabularLines: ({ details: true, narrowAlbum: true, narrowDetails: true })

    // The words of `text` (what is between its spaces) that are not empty,
    // each as [start, end], with `offset` added.
    function _wordRanges(text, offset) {
        const ranges = []
        let start = offset
        for (const word of text.split(" ")) {
            if (word.length > 0)
                ranges.push([start, start + word.length])
            start += word.length + 1
        }
        return ranges
    }

    // Whether two words take the same width in tabular figures: the same
    // but for digits, each in place of another.
    function _sameWidth(left, right) {
        if (left.length !== right.length)
            return false
        for (let index = 0; index < left.length; ++index) {
            if (left[index] !== right[index]
                    && !(/[0-9]/.test(left[index]) && /[0-9]/.test(right[index])))
                return false
        }
        return true
    }

    // What stays still while a line goes from `before` to `after`: the
    // runs of words of `after`, as [start, end] in order, that both have,
    // the same, in the same place. A field that is the same stays still
    // while what is before it on the line keeps its width; within a field
    // in which only numbers change, so do the words that are the same. In
    // tabular figures (`tabular`), a number with as many digits keeps its
    // width. A field or word that moves fades, and so does all after it.
    // Nothing stays still in right-to-left text, whose fields are not laid
    // out from the left.
    function _stillRuns(before, after, separator, tabular) {
        if (root._hasRightToLeft(before) || root._hasRightToLeft(after))
            return []
        if (before === after)
            return before.length > 0 ? [[0, before.length]] : []
        const left = separator.length > 0 ? before.split(separator) : [before]
        const right = separator.length > 0 ? after.split(separator) : [after]
        const words = []
        let at = 0
        for (let field = 0; field < left.length && field < right.length; ++field) {
            // Both go on past the separator before this field, which is in
            // the same place in both.
            if (field > 0)
                words.push(...root._wordRanges(separator, at - separator.length))
            const was = left[field]
            const is = right[field]
            if (was !== is) {
                if (was.replace(/[0-9]+/g, "0") !== is.replace(/[0-9]+/g, "0"))
                    break
                const wasWords = was.split(" ")
                const isWords = is.split(" ")
                let offset = at
                let kept = true
                for (let word = 0; kept && word < isWords.length; ++word) {
                    if (wasWords[word] === isWords[word])
                        words.push(...root._wordRanges(isWords[word], offset))
                    else
                        kept = tabular && root._sameWidth(wasWords[word], isWords[word])
                    offset += isWords[word].length + 1
                }
                if (!kept)
                    break
            } else {
                words.push(...root._wordRanges(is, at))
            }
            at += is.length + separator.length
        }
        // Words with nothing but spaces between them stay still as one.
        const runs = []
        for (const word of words) {
            const last = runs.length > 0 ? runs[runs.length - 1] : null
            if (last !== null && /^ *$/.test(after.slice(last[1], word[0])))
                last[1] = word[1]
            else
                runs.push([word[0], word[1]])
        }
        return runs
    }

    function _stillRunsOf(key, before, after) {
        if (root.wrappingLines[key] === true || root._isFolderLine(before) || root._isFolderLine(after))
            return []
        return root._stillRuns(before, after, root.lineSeparators[key] || "", root.tabularLines[key] === true)
    }

    // The lines that wrap onto a second line rather than be cut off, and
    // so fade whole: in the narrow layout the title (beside the cover at
    // 24 pt a long title was cut off), under the narrow cover the
    // artist too, under a title on one line, and under it, in the room the
    // title and the artist leave, and beside it in the compact layout the
    // album with its year (at 24 pt a long album kept a fifth of its
    // text), but under the large-font header (largeHeader),
    // where it has the whole width, one line, its year whole after it.
    // Beside the compact cover, and under the header, the artist takes one
    // line, elided, with the whole of it in the tooltip. The
    // album line fades whole wherever it shows, as its year stands apart
    // from it (_albumAndYear).
    readonly property var wrappingLines: root.wideRoom ? ({})
        : (root.narrowStacked ? ({ title: true, artist: true, narrowAlbum: true })
                              : ({ title: true, narrowAlbum: true }))
    readonly property bool narrowLinesWrap: (root.compactLayout && !root.largeHeader)
        || (root.narrowStacked && narrowTitle.textLineCount < 2 && narrowArtist.textLineCount < 2)
    readonly property int stackedLineGap: 4

    // Whether the line of `key` fades whole, as one, going from `before` to
    // `after` while `still` of it stays still: when its structure changes,
    // as it has more or fewer fields, or a field with a number more or fewer
    // words ("Track 2" to "Track 2 of 8"); when what changes is in more than
    // one place of it; or when less than half of its glyphs would stay
    // still. A line in which only a word or a number changes fades just
    // that; otherwise the words that would stay would show alone while the
    // rest of the line is gone.
    function _fadesWhole(key, still, before, after) {
        const separator = root.lineSeparators[key] || ""
        const fieldsBefore = separator.length > 0 ? before.split(separator) : [before]
        const fieldsAfter = separator.length > 0 ? after.split(separator) : [after]
        if (fieldsBefore.length !== fieldsAfter.length)
            return true
        for (let field = 0; field < fieldsAfter.length; ++field) {
            if (/[0-9]/.test(fieldsBefore[field] + fieldsAfter[field])
                    && fieldsBefore[field].split(" ").length !== fieldsAfter[field].split(" ").length)
                return true
        }
        let glyphs = 0
        let stillGlyphs = 0
        let changes = 0
        let changing = false
        for (let index = 0; index < after.length; ++index) {
            if (after[index] === " ")
                continue
            ++glyphs
            if (still.some(run => index >= run[0] && index < run[1])) {
                ++stillGlyphs
                changing = false
            } else {
                changes += changing ? 0 : 1
                changing = true
            }
        }
        return changes > 1 || 2 * stillGlyphs < glyphs
    }

    // What stays still of the line of `key` going from `before` to `after`,
    // of `still`, what stayed still of it so far: no more, and nothing once
    // the line fades whole.
    function _keptStill(key, still, before, after) {
        const kept = root._commonRuns(still, root._stillRunsOf(key, before, after))
        return root._fadesWhole(key, kept, before, after) ? [] : kept
    }

    // The runs both `left` and `right` have.
    function _commonRuns(left, right) {
        const common = []
        for (const one of left) {
            for (const other of right) {
                const start = Math.max(one[0], other[0])
                const end = Math.min(one[1], other[1])
                if (start < end)
                    common.push([start, end])
            }
        }
        return common
    }

    function _sameRuns(left, right) {
        return left.length === right.length
            && left.every((run, index) => run[0] === right[index][0] && run[1] === right[index][1])
    }

    // What of `runs`, runs of words of `text`, is not in `kept`, a part of
    // them: from the gap before each piece to the gap after it, as [at,
    // at] in the terms of `_layer`.
    function _givenUp(runs, kept, text) {
        const spans = []
        const add = (start, end) => {
            while (start < end && text[start] === " ")
                ++start
            while (end > start && text[end - 1] === " ")
                --end
            if (start < end)
                spans.push([start === 0 ? 0 : start - 1, end])
        }
        for (const run of runs) {
            let start = run[0]
            for (const keep of kept) {
                if (keep[1] <= start || keep[0] >= run[1])
                    continue
                add(start, keep[0])
                start = Math.max(start, keep[1])
            }
            add(start, run[1])
        }
        return spans
    }

    // `parts` (see `_layer`), with `from` from `at` up to `until`.
    function _withFrom(parts, at, until, from) {
        const after = parts.filter(part => part.at <= until).pop().from
        const result = parts.filter(part => part.at < at)
        result.push({ at: at, from: from })
        if (!parts.some(part => part.at === until))
            result.push({ at: until, from: after })
        return result.concat(parts.filter(part => part.at >= until))
    }

    // The line of `key` in a fade that starts now, as it shows when the
    // playing track's line goes from `before` to `after`; null when it
    // does not fade. Each layer goes on from the opacity each of its parts
    // has. During a track change (`track`), the playing track's line
    // before goes out as a layer of its own when it was the track
    // before's, from the opacity it has, as far as it faded in; a line
    // that did not fade, from full opacity. A line that is the playing
    // track's own changes in place, as late metadata does, and so does any
    // line once it fades out as many layers as it can. What stayed still
    // and `after` no longer has in its place fades out from full opacity,
    // as it showed, in the newest layer, and the playing track's text there
    // fades in with the rest of it; so that the playing track's text there
    // does not show at once at the opacity the rest of it has come in to,
    // the playing track's line, once it has begun to come in, then goes out
    // as a layer of its own too, from where it had come in to, and comes in
    // again from nothing.
    // `newTrack`: the fade is another track's, whose lines are still the
    // track before's.
    function _nextLine(key, before, after, track, newTrack) {
        const changes = before !== after
        const line = root._shownLine(key)
        if (line === null) {
            if (!track || !changes)
                return null
            const whole = [[0, Math.max(before.length, after.length)]]
            return root._fadingLine(root._keptStill(key, whole, before, after),
                                    [root._layer(before, [{ at: 0, from: 1 }])], 1, true)
        }
        const level = root._lineOutLevel(line)
        const still = root._keptStill(key, line.still, before, after)
        const givenUp = root._givenUp(line.still, still, before)
        let layers = line.layers
            .map(layer => root._layer(layer.text, layer.parts.map(part => ({ at: part.at, from: part.from * level }))))
            .filter(layer => root._layerFrom(layer) > 0)
        const own = !newTrack && line.own
        let incomingFrom = root._incomingLineRest(key)
        // What stayed still and goes out needs a layer to go out in, and
        // the playing track's text there must come in from nothing, not
        // from where the rest of it has come in to.
        const givesUp = givenUp.length > 0 && (incomingFrom < 1 || layers.length === 0)
        if (changes && (!own || givesUp) && layers.length < root.maxOutgoingLayers) {
            layers.push(root._layer(before, [{ at: 0, from: 1 - incomingFrom }]))
            incomingFrom = 1
        }
        // Every layer had what was still in common with the playing track's
        // text: the newest draws it, the others nothing there.
        layers = layers.map((layer, index) => root._layer(layer.text, root._mergedParts(givenUp.reduce(
            (parts, span) => root._withFrom(parts, span[0], span[1], index === layers.length - 1 ? 1 : 0),
            layer.parts))))
            .filter(layer => root._layerFrom(layer) > 0)
        // A line with nothing before it left to go out may still be coming
        // in: it goes on from where it is.
        return layers.length > 0 || incomingFrom > 0
            ? root._fadingLine(still, layers, incomingFrom, own || changes) : null
    }

    // Parts that fade alike are one.
    function _mergedParts(parts) {
        return parts.filter((part, index) => index === 0 || part.from !== parts[index - 1].from)
    }

    // What a track shows in the cover's place: its cover, or the stand-in
    // for one, which is the same for every track without a cover.
    function _coverKey(track) {
        return track.coverPath.length > 0 ? "cover\n" + track.coverPath : "standIn"
    }

    // The covers before in a fade that starts now, as they show when the
    // playing track's cover goes from `was`'s to `next`'s: each goes on
    // from the opacity it has, and the cover that showed whole, or the
    // stand-in for one, goes out from there, under them, over the playing
    // track's, when that differs: a cover over the same one adds nothing.
    // A cover right over the same one adds nothing either, so the one it
    // replaces goes.
    function _nextCovers(was, next) {
        const level = root._coversOutLevel(root.handover)
        const covers = root.outgoing === null ? [] : root.outgoing.covers
            .map(cover => ({ coverPath: cover.coverPath, from: cover.from * level,
                             key: cover.key }))
            .filter(cover => cover.from > 0)
        const key = root._coverKey(was)
        if (key !== root._coverKey(next)) {
            const newest = covers.length > 0 ? covers[covers.length - 1] : null
            if (newest !== null && newest.key === key)
                covers.pop()
            if (covers.length < root.maxOutgoingLayers)
                covers.push({ coverPath: was.coverPath, from: 1, key: key })
        }
        return covers
    }

    function followTrack() {
        const next = {
            trackKey: root._bridgeTrackKey(),
            album: root.albumText,
            coverPath: root.coverPath,
            lines: root.trackLines
        }
        const was = root.shown
        root.shown = next
        // A track, as it shows, for the panel to hold as it shrinks: only
        // one with a path, as the bridge clears the path with the rest.
        if (next.trackKey.length > 0)
            root.heldTrack = { coverPath: next.coverPath, lines: next.lines }
        if (was === null || was.trackKey.length === 0 || next.trackKey.length === 0) {
            // Into or out of "Nothing playing": at once, as the idle
            // placeholder is not a cover nor its lines a track's.
            root.endHandover()
            const wasEmpty = was !== null && was.trackKey.length === 0
            if (next.trackKey.length === 0) {
                root.settling = false
                if (!wasEmpty) {
                    root.coverCameWhileEmpty = false
                } else if (was.coverPath !== next.coverPath) {
                    root.coverCameWhileEmpty = next.coverPath.length > 0
                }
            } else {
                // The track's cover is yet to come, unless it came first.
                root.settling = !root.coverCameWhileEmpty
                root.coverCameWhileEmpty = false
            }
            return
        }
        if (was.trackKey !== next.trackKey) {
            root.settling = false
            root.handOver(was, next, true, true)
            return
        }
        // The cover, or the stand-in for one, whose texts follow the tags.
        const coverChange = root._coverKey(was) !== root._coverKey(next)
        if (root.settling) {
            // The first track's metadata, in place, until its cover.
            if (was.coverPath !== next.coverPath) {
                root.settling = false
            }
            return
        }
        const fading = root.outgoing
        if (coverChange || (fading !== null && !fading.long && was.album !== next.album)
                || (fading !== null && fading.track && root._linesJoin(was, next))) {
            // A cover that changed, an album named after the track itself
            // while only its lines fade, or a line of the track change
            // that comes to differ and has to join the fade.
            root.handOver(was, next, fading !== null && fading.track, false)
        }
    }

    // Whether a line changed from `was` to `next` that does not change in
    // place: one that does not fade, one that fades but shows the track
    // before's text, or one that no longer has all that stayed still of it
    // in place.
    function _linesJoin(was, next) {
        for (const key in next.lines) {
            if (was.lines[key] === next.lines[key])
                continue
            const line = root._shownLine(key)
            if (line === null || !line.own)
                return true
            if (!root._sameRuns(root._keptStill(key, line.still, was.lines[key], next.lines[key]), line.still))
                return true
        }
        return false
    }

    // The fade starts again from what shows, for another track (`newTrack`)
    // or, for the same one, its cover, album or a line that changed: every
    // layer goes on from the opacity it has, on the long curve once the
    // cover or the album changed. No layer jumps. A track change runs even before any
    // of its lines differs, for the lines its metadata changes once it
    // comes.
    function handOver(was, next, track, newTrack) {
        const lines = {}
        for (const key in next.lines) {
            const line = root._nextLine(key, was.lines[key], next.lines[key], track, newTrack)
            if (line !== null)
                lines[key] = line
        }
        const covers = root._nextCovers(was, next)
        const out = {
            lines: lines,
            covers: covers,
            track: track,
            long: covers.length > 0 || was.album !== next.album
                || (root.outgoing !== null && root.outgoing.long)
        }
        const duration = out.long ? Kirigami.Units.longDuration : Kirigami.Units.shortDuration
        const fades = out.track || covers.length > 0 || Object.keys(out.lines).length > 0
            || out.long
        if (!fades || duration <= 0) {
            root.endHandover()
            return
        }
        root.handoverAnimation.stop()
        root.outgoing = out
        root.handoverAnimation.duration = duration
        root.handover = 0
        root.handoverAnimation.start()
    }

    function endHandover() {
        root.handoverAnimation.stop()
        root.outgoing = null
        root.handover = 1
    }

    onTrackLinesChanged: root.followTrack()
    onTrackKeyChanged: root.followTrack()
    onCoverPathChanged: root.followTrack()
    Component.onCompleted: {
        // What shows at the start is the start: nothing grows into it.
        root.presenceAnimation.stop()
        root.presence = root.idle ? 0 : 1
        root.followTrack()
    }

    // The title, over the text under it: under the wide cover a level 1
    // Kirigami.Heading's size, 13.5 pt with the default 10 pt font (a step
    // up from level 2, to stand to the 360 px cover), and in the narrow
    // layout a level 2's, 12 pt. It follows the font.
    readonly property real wideTitlePointSize: root.uiPalette.headingPointSize(1)
    readonly property real titlePointSize: root.uiPalette.headingPointSize(2)

    // The wide text block, under the cover, is on the queue's row grid:
    // with the cover it is a whole number of the list's rows
    // high, UiPalette's rowHeight, which the queue's rows are too, so that
    // the seam at its foot and the hairline under the library's header
    // fall on the lines of the queue's rows; with the default font and the
    // 360 px sidebar the block is 82 px, and the header's hairline
    // continues the seam between the queue and the visualization. The
    // lines start `wideTextTop` under the cover, whatever the track, and
    // what the grid leaves over is under them, at least `wideLeastGap`
    // before the seam, which sets the panel and the library apart as
    // regions of one sheet (28 px there left a dead band).
    readonly property int wideTextTop: 2 * Kirigami.Units.smallSpacing
    readonly property int wideLeastGap: 2 * Kirigami.Units.smallSpacing
    function _wideBlockHeight(linesHeight) {
        const row = root.uiPalette.rowHeight
        const least = root.width + root.wideTextTop + linesHeight + root.wideLeastGap
        return row > 0 ? Math.ceil(least / row) * row - root.width : least - root.width
    }
    // The 1 px seam at the panel's foot, where the library starts.
    readonly property int seamHeight: 1
    // The top of the lines beside the narrow cover; beside the large-font
    // header's cover, at its top (besideTextTop).
    readonly property int narrowTextTop: 10
    readonly property int besideTextTop: root.largeHeader ? root.coverInset
        : root.narrowBeside ? root.centredTextTop : root.narrowTextTop
    // The top of the lines beside a cover that fills the block, one for
    // every track, from the room they keep (narrowReservedHeight): a
    // title's four lines centred on the cover, as far as the second line
    // of a title on two, which that room is for, leaves besideLeastFoot
    // over the seam (10 px down, the common four stood 14 px under the
    // cover's top and 36 over its foot; centred whole, a title on two
    // lines set the details on the seam).
    readonly property int besideLeastFoot: 6
    readonly property int centredTextTop: Math.max(root.narrowTextTop, Math.round(Math.min(
        (root.besideCoverSize - (root.narrowReservedHeight - narrowTitleProbe.implicitHeight / 2)) / 2,
        root.besideCoverSize - root.narrowReservedHeight - root.besideLeastFoot)))
    // The narrow lines' top once in place: under the cover, or beside it.
    readonly property real narrowColumnTop: root.narrowStacked
        ? root.coverSize + root.textInset : root.besideTextTop
    // Where the line under the cover starts down: under the cover's
    // place, and while the track's layout grows from the compact
    // row, or shrinks into it, never above the row's foot, so the lines do
    // not show where the row's text does.
    readonly property real footTop: Math.max(coverArea.height,
        Math.round(root.lineShift * root.idleRowHeight))
    // The least width of the lines beside the narrow cover: about 12
    // characters of the font.
    readonly property real narrowLineMinimum: 12 * baseMetrics.averageCharacterWidth
    // How tall the four lines are beside the narrow cover, from its top.
    readonly property real narrowLinesHeight: root.narrowTextTop + narrowTitleMetrics.height
        + root.narrowTitleGap + 3 * narrowLineMetrics.height
    // Beside the narrow cover, the gap between the title and the artist
    // under it, which sets the title apart from the three small lines that
    // are one group (2 px, the same as between those, ran them
    // together). The block stays the cover's height and the grid's.
    readonly property int narrowTitleGap: 4
    // A font too large for the four lines beside the 96 px cover, as 24 pt:
    // from the fonts alone, so a track does not switch it.
    readonly property bool largeFont: root.narrowLinesHeight > root.narrowCoverSize
    // Whether, in a narrow sidebar, the four lines would run well past the
    // 96 px cover beside it, as with 24 pt (at 12 and 14 pt they run past
    // it by a few pixels, and stay beside it).
    readonly property bool narrowLinesTall: root.narrowLinesHeight > 1.25 * root.narrowCoverSize
    // The large-font header, in a narrow sidebar and in one wide enough for
    // the wide layout that takes this one for its height alone alike
    // (at 24 pt the narrow sidebar stood a 96 px cover alone on a row,
    // some 264 px of it empty beside it, and the compact one cut the artist
    // off beside a 152 px cover): one block on one left edge, the panel's
    // inset. The cover, as high as two lines of the title (headerCoverSize),
    // at the inset from the top too, and beside it, besideGap after it, the
    // panel's inset (8 px read cramped against the cover at 24 pt, tighter
    // than every other gap in the block), the title, to headerTitleEnd
    // from the panel's right side, so it keeps the room it had (with 12 px
    // and the inset on the right, "A Remarkably" was past the 408 px
    // sidebar's first line, "A / Remarkably…", and a long title showed two
    // words),
    // on up to two lines and centred on it either way (flush in the corner
    // with the title on its foot, the header had three left edges and some
    // 60 px of nothing over the title); then, footGap, the panel's inset,
    // under the cover (8 px there read tight under the 24 pt title and the
    // cover over it), on its left edge and to the panel's inset, the
    // artist, "Album (Year)", the album cut off before its year, in
    // textSecondary, and the details, a line each, elided, stackedLineGap
    // apart. The block is one height for every track (largeHeaderHeight): a
    // line a track has no text for keeps its row, so a track change does
    // not move the library, and a file without tags has its folder and its
    // facts in the first two, as a tagged track's artist and album, the
    // spare row at the foot (shifted to the middle of the three, the folder
    // stood ~30 px under the cover and moved on a change to a tagged
    // track). Where the
    // title would have under narrowLineMinimum beside the cover, the lines
    // go under it instead (narrowStacked).
    readonly property bool largeHeader: !root.wideRoom && root.largeFont
        && (!root.narrow || root.narrowLinesTall)
        && root._roomBesideHeader(root.headerCoverSize) >= root.narrowLineMinimum
    readonly property int besideGap: root.textInset
    // Where the header's title ends, from the panel's right side: a ragged
    // right edge, which no line under it lines up with, so 8 px, Kirigami's
    // two small spacings, rather than the inset, which the gap before it
    // takes.
    readonly property int headerTitleEnd: 2 * Kirigami.Units.smallSpacing
    readonly property int footGap: root.textInset
    readonly property int titleGapBeside: root.largeHeader ? root.besideGap : root.textInset
    readonly property real footLineWidth: Math.max(0, root.width - 2 * root.textInset)
    readonly property real largeHeaderHeight: root.textInset + root.headerCoverSize + root.footGap
        + 3 * narrowLineProbe.implicitHeight + 2 * root.stackedLineGap
    // The lines go under the cover, on the whole width, where the room
    // beside it is under narrowLineMinimum, or where, in a narrow sidebar,
    // they would run well past the cover and the large-font header has no
    // room: there the room beside it would still show only a dozen
    // characters of each, under the cover they show more, and the title
    // and the artist wrap rather than be cut off. A sidebar wide enough for
    // the wide layout that takes this one for its height alone, as with a
    // large font in a 1000 px high window, keeps the lines beside the
    // cover, or the header: the room beside it is wide enough there, and
    // under it the cover stood alone on a row, with its width empty beside
    // it.
    readonly property bool narrowStacked: !root.wideRoom && !root.largeHeader
        && ((root.largeFont && !root.narrow ? root._roomBesideHeader(root.headerCoverSize)
                                            : root._roomBeside(root.narrowCoverSize))
                < root.narrowLineMinimum
            || (root.narrow && root.narrowLinesTall))
    // The compact layout: the narrow one in a sidebar wide enough for the
    // wide layout, which takes this one for its height alone; with a large
    // font, the large-font header. Otherwise the lines are beside the
    // 96 px cover, where the album with its year takes two lines where it
    // needs them, as the title does (at 24 pt a long album was cut off on
    // one line, its year lost), the artist one, elided, and the block keeps
    // the room of them all, six lines: two each for the title and the
    // album, one each for the artist and the details, whatever the track,
    // so a track change does not move the library (two for the artist too
    // left some 145 px blank under a common track).
    readonly property bool compactLayout: !root.wideRoom && !root.narrow && !root.narrowStacked
    readonly property real compactLinesHeight: root.narrowTextTop + narrowTitleProbe.implicitHeight
        + root.narrowTitleGap + narrowTwoLinesProbe.implicitHeight + 2 * narrowLineProbe.implicitHeight
    // In a narrow sidebar, the four lines beside the cover, which fills the
    // block (besideCoverSize).
    readonly property bool narrowBeside: !root.wideRoom && root.narrow && !root.narrowStacked && !root.largeHeader

    FontMetrics {
        id: narrowTitleMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.titlePointSize
        font.weight: Font.DemiBold
    }
    FontMetrics {
        id: narrowLineMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
    }

    FontMetrics {
        id: baseMetrics
        font: root.uiPalette.baseFont
    }

    // A line of the wide layout's title, of its artist and album, and of
    // its details, as tall as their text lays them out, whatever the track:
    // the wide layout's height.
    component LineProbe: Text {
        visible: false
        text: "X"
        textFormat: Text.PlainText
        font.family: root.uiPalette.baseFont.family
        Accessible.ignored: true
    }
    LineProbe {
        id: wideTitleProbe
        font.pointSize: root.wideTitlePointSize
        font.weight: Font.DemiBold
    }
    LineProbe {
        id: wideLineProbe
        font.pointSize: root.uiPalette.basePointSize
    }
    LineProbe {
        id: wideDetailsProbe
        font.pointSize: root.uiPalette.smallPointSize
    }
    // Two lines of the narrow title and of the small lines, and one of
    // these, which the compact layout keeps room for, and which the compact
    // header's cover spans (two of the title, one small one).
    LineProbe {
        id: narrowTitleProbe
        text: "X\nX"
        font.pointSize: root.titlePointSize
        font.weight: Font.DemiBold
    }
    // Two lines of the large-font header's title at the base size, which
    // its cover then spans (headerTitleSmall).
    LineProbe {
        id: headerSmallTitleProbe
        text: "X\nX"
        font.pointSize: root.uiPalette.basePointSize
        font.weight: Font.DemiBold
    }
    LineProbe {
        id: narrowTwoLinesProbe
        text: "X\nX"
        font.pointSize: root.uiPalette.smallPointSize
    }
    LineProbe {
        id: narrowLineProbe
        font.pointSize: root.uiPalette.smallPointSize
    }

    // The tile with nothing in it, at rest under the compact row: the
    // sidebar's view colour, as one surface with it (a lighter tile read
    // as a broken image).
    readonly property color placeholderColor: root.uiPalette.view

    // The room the narrow lines keep, beside the cover or under it,
    // whatever the track has text for, from the fonts alone (following
    // the track's text, a title on two lines, or an album on two under the
    // cover, moved the library): two lines of the title and
    // one each of the artist, the album and the details, with the gaps
    // between them. Under the cover, a title on one line leaves the artist,
    // or else the album, a second line in the title's line it does not
    // take (stackedSpareLine), as a small line is no taller than one of
    // the title.
    readonly property real narrowReservedHeight: narrowTitleProbe.implicitHeight
        + 3 * narrowLineProbe.implicitHeight
        + (root.narrowStacked ? 3 * root.stackedLineGap : root.narrowTitleGap)
    // The block, one height for every track: under the cover, the cover,
    // the gap and the lines' room; the large-font header's; the compact
    // layout's cover or six lines, whichever is taller; beside the narrow
    // cover, which fills it, the 96 px cover or the lines' room, whichever
    // is taller (besideBlockSize). Then 12 px, but beside the narrow cover,
    // which is flush with the seam, and the seam.
    readonly property real narrowLayoutHeight: root.narrowStacked
            ? root.narrowCoverSize + root.textInset + root.narrowReservedHeight + root.textInset + root.seamHeight
        : root.largeHeader ? root.largeHeaderHeight + root.textInset + root.seamHeight
        : root.compactLayout ? Math.max(root.narrowCoverSize, root.compactLinesHeight) + root.textInset + root.seamHeight
        : root.besideBlockSize + root.seamHeight
    // The wide block's height: its three lines', whatever the track has
    // text for, from the font alone.
    readonly property real wideLinesHeight: wideTitleProbe.implicitHeight + wideColumn.spacing
        + wideLineProbe.implicitHeight + wideColumn.spacing + wideDetailsProbe.implicitHeight
    readonly property real wideBlockHeight: root._wideBlockHeight(root.wideLinesHeight)
    readonly property real wideLayoutHeight: root.width + root.wideBlockHeight + root.seamHeight
    readonly property real layoutHeight: root.wideRoom ? root.wideLayoutHeight : root.narrowLayoutHeight
    // The compact row with nothing loaded, the block with a track, and
    // between them while one grows into the other.
    implicitHeight: root._whileGrowing(root.restHeight
        + (root.movedHeight - root.restHeight) * root.presence)
    // While it grows or shrinks, what is past its foot does not show.
    clip: root.presence < 1

    Accessible.role: Accessible.Grouping
    Accessible.name: I18n.i18nc("@label", "Now playing")

    function _trimmed(value) {
        return String(value || "").trim()
    }

    function _joined(parts, separator) {
        return parts.filter(part => part.length > 0).join(separator)
    }

    // The hyphen a word broken between lines ends its first part with:
    // U+2010, which is never a minus, nor a place to break again.
    readonly property string wordBreakHyphen: "\u2010"
    // The least of a word on either side of its hyphen, in characters.
    readonly property int hyphenatedHead: 4
    readonly property int hyphenatedTail: 3

    // `text` as a line lays it out onto `lines` lines `width` wide in the
    // font of `metrics`, the last `lastWidth` wide, {text, elided}. A word
    // wider than the line, where the line `breaksLongWords`, breaks with a
    // hyphen where its first part fills the line, at least hyphenatedHead
    // characters of it before the hyphen and hyphenatedTail after it, the
    // line ending there ("\n"), rather than anywhere ("A Remarka / bly
    // Long …" split the word with no mark); the lines before the last as
    // Text wraps them. A word only the ellipsis cuts is cut as any line's
    // last word is. Where Text would elide the last line after a space,
    // "Title That …", the panel elides it instead, without the space, the
    // lines before it ending in "\n", and `elided` is set; a title's
    // second line it elides itself either way, as Text can cut some px
    // short there and leave the space ("Long Track …" where "Long Track T…"
    // fitted); otherwise Text elides it. `text` as it is where
    // nothing of this applies, and where a word wider than the line has no
    // such break, which Text then breaks anywhere.
    function _laidOut(text, width, lines, lastWidth, breaksLongWords, metrics) {
        // Read the font, for the answer to follow it.
        metrics.font
        const whole = { text: text, elided: false }
        if (width <= 0 || lastWidth <= 0 || text.length === 0)
            return whole
        const fits = (line, room) => metrics.advanceWidth(line) <= room
        if (lines <= 1)
            return fits(text, lastWidth) ? whole : root._elidedWithoutSpace(text, [], lastWidth, metrics) || whole
        const words = text.split(/\s+/).filter(word => word.length > 0)
        const laid = []
        let current = ""
        let index = 0
        let broken = false
        while (index < words.length && laid.length < lines - 1) {
            const word = words[index]
            const joined = current.length > 0 ? current + " " + word : word
            if (fits(joined, width)) {
                current = joined
                ++index
                continue
            }
            if (fits(word, width)) {
                // The word goes onto the next line.
                laid.push(current)
                current = ""
                continue
            }
            // A word wider than a line, cut on this one or the next.
            const at = breaksLongWords ? root._hyphenPoint(word, current, line => fits(line, width)) : 0
            if (at <= 0)
                return whole
            laid.push((current.length > 0 ? current + " " : "") + word.slice(0, at) + root.wordBreakHyphen)
            words[index] = word.slice(at)
            current = ""
            broken = true
        }
        const rest = [current].concat(words.slice(index)).filter(part => part.length > 0).join(" ")
        const brokenText = { text: laid.concat(rest.length > 0 ? [rest] : []).join("\n"), elided: false }
        // Ending on a line before the last, at the line's whole width, or
        // fitting the last.
        if (laid.length < lines - 1 || fits(rest, lastWidth))
            return broken ? brokenText : whole
        const trimmed = root._elidedWithoutSpace(rest, laid, lastWidth, metrics)
        if (trimmed !== null || !breaksLongWords)
            return trimmed || whole
        return { text: laid.concat([metrics.elidedText(rest, Qt.ElideRight, lastWidth)]).join("\n"), elided: true }
    }

    // `text`, elided on the right to `width` in the font of `metrics` after
    // the lines `before`, as {text, elided}, where Text would leave a space
    // before the ellipsis: "Long Track T…", "Title That…" rather than "Title
    // That …"; null where it would not.
    function _elidedWithoutSpace(text, before, width, metrics) {
        const elided = metrics.elidedText(text, Qt.ElideRight, width)
        let kept = 0
        while (kept < elided.length && kept < text.length && elided[kept] === text[kept])
            ++kept
        const head = elided.slice(0, kept)
        const trimmed = head.replace(/\s+$/, "")
        if (kept >= elided.length || trimmed === head)
            return null
        return { text: before.concat([trimmed + elided.slice(kept)]).join("\n"), elided: true }
    }

    // Where `word` breaks with a hyphen after `before`, the start of its
    // line, so that the line fills (`fits`): the most characters of it
    // that fit with the hyphen, at least hyphenatedHead and leaving at least
    // hyphenatedTail, never within a surrogate pair; 0 for none.
    function _hyphenPoint(word, before, fits) {
        const start = before.length > 0 ? before + " " : ""
        for (let at = word.length - root.hyphenatedTail; at >= root.hyphenatedHead; --at) {
            const code = word.charCodeAt(at)
            if (code >= 0xDC00 && code <= 0xDFFF)
                continue
            if (fits(start + word.slice(0, at) + root.wordBreakHyphen))
                return at
        }
        return 0
    }

    // A text of a line (`wholeText`), as each of its layers draws it:
    // elided, on the whole width of the line, so the layers lay it out
    // alike. Where the line keeps a tail of its text whole, as the album's
    // year, the rest is the Text's `text` and the tail is drawn after it,
    // on the last line, which is that much shorter, so what is cut off is
    // cut off before the tail; or, where the rest ends on a line before
    // the last with no room after it, at the start of the next line.
    component LineText: Text {
        id: lineText

        required property var owner
        required property string wholeText
        // A file's folder rather than a tag (see folderText): without its
        // mark, in textSecondary. A file's facts are without theirs, in the
        // details' textTertiary in whichever row they are.
        readonly property bool folderLine: root._isFolderLine(lineText.wholeText)
        readonly property string shownText: lineText.folderLine
            ? lineText.wholeText.slice(root.folderMark.length)
            : root._isFactsLine(lineText.wholeText)
                ? lineText.wholeText.slice(root.factsMark.length) : lineText.wholeText
        readonly property var parts: lineText.owner.keepsTail
            ? root._albumAndYear(lineText.shownText) : [lineText.shownText, ""]
        readonly property string tail: lineText.parts[1]
        readonly property real tailWidth: lineText.tail.length > 0
            ? Math.ceil(tailMetrics.advanceWidth(lineText.tail)) : 0
        // The last line as laid out: where it ends, its top and foot and
        // its number.
        property real lastLineEnd: 0
        property real lastLineTop: 0
        property real lastLineBottom: 0
        property int lastLineNumber: 0
        readonly property bool tailOnItsOwnLine: lineText.tail.length > 0
            && lineText.lastLineNumber < lineText.maximumLineCount - 1
            && lineText.lastLineEnd + lineText.tailWidth > lineText.width
        // The height the tail adds on a line of its own.
        readonly property real tailLineHeight: lineText.tailOnItsOwnLine ? tailText.implicitHeight : 0

        // The text without its tail, before any hyphen or ellipsis.
        readonly property string bodyText: lineText.parts[0]
        // Laid out by the panel where it is cut off or breaks a word
        // (_laidOut), the last line as long as the tail leaves it.
        readonly property var laidOut: root._laidOut(lineText.bodyText, lineText.width,
            lineText.owner.maximumLineCount, Math.max(0, lineText.width - lineText.tailWidth),
            lineText.owner.breaksLongWords, tailMetrics)
        // Whether the text is cut off: by the panel, or by Text itself.
        readonly property bool elided: lineText.laidOut.elided || lineText.truncated
        text: lineText.laidOut.text
        width: owner.width
        // In the middle of the line's seat, where it has one (see
        // FadingLine's seatHeight): each text by its own height, so a layer
        // that fades out stays where it showed. By the height of the
        // laid-out text: reading implicitHeight here would lay it out from
        // within this binding, a binding loop.
        y: owner.seatHeight > 0
            ? Math.max(0, Math.floor((owner.seatHeight - lineText.contentHeight) / 2)) : 0
        elide: Text.ElideRight
        maximumLineCount: owner.maximumLineCount
        wrapMode: owner.maximumLineCount <= 1 ? Text.NoWrap : Text.Wrap
        textFormat: Text.PlainText
        font.family: root.uiPalette.baseFont.family
        font.pointSize: owner.pointSize
        font.weight: owner.weight
        font.features: owner.features
        color: lineText.folderLine ? root.uiPalette.textSecondary
            : root._isFactsLine(lineText.wholeText) ? root.uiPalette.textTertiary : owner.color

        // Each line in turn, on the line's width, which a text with this
        // handler is elided on only where it is set; the last call is for
        // the last line, as drawn.
        onLineLaidOut: function(line) {
            const last = line.number === lineText.maximumLineCount - 1
            line.width = Math.max(0, lineText.width - (last ? lineText.tailWidth : 0))
            if (lineText.tailWidth > 0) {
                lineText.lastLineEnd = line.x + line.implicitWidth
                lineText.lastLineTop = line.y
                lineText.lastLineBottom = line.y + line.height
                lineText.lastLineNumber = line.number
            }
        }
        onTailWidthChanged: lineText.forceLayout()

        FontMetrics {
            id: tailMetrics
            font: lineText.font
        }

        Text {
            id: tailText
            objectName: "nowPlayingLineTail"
            x: lineText.tailOnItsOwnLine ? 0 : lineText.lastLineEnd
            y: lineText.tailOnItsOwnLine ? lineText.lastLineBottom : lineText.lastLineTop
            visible: lineText.tail.length > 0
            // At a line's start without the space before it.
            text: lineText.tailOnItsOwnLine ? lineText.tail.replace(/^\s+/, "") : lineText.tail
            textFormat: Text.PlainText
            font: lineText.font
            color: lineText.color
            Accessible.ignored: true
        }
    }

    // A line of the playing track, with its text as the accessible name.
    // While a track change fades it, the words that every text it shows has
    // the same, in the same place, stay still, drawn by the playing track's
    // own text, clipped, so nothing in them moves or dims. Elsewhere the
    // playing track's text fades in as one, over the lines before, the
    // oldest at the bottom; each part of a line before fades out on its own
    // (see `_layer`), between edges in its own gaps.
    component FadingLine: Item {
        id: line

        // What the line needs under the wide cover, down to its foot, or
        // -1 in the narrow layouts; where its text's foot is, from the
        // panel's top; and so how much of it shows as the panel grows or
        // shrinks (_lineLevel).
        property real roomNeeded: -1
        readonly property real footInColumn: line.y
            + stillText.y + stillText.height + stillText.tailLineHeight
        readonly property real foot: (line.parent ? line.parent.y : 0) + line.footInColumn
        readonly property real placedFoot: root.narrowColumnTop + line.footInColumn
        opacity: root._lineLevel(line.name, line.foot, line.placedFoot, line.roomNeeded)

        // The line's key in `trackLines`.
        required property string lineKey
        required property string text
        // Its items' names: "nowPlaying<name>" for the playing track's
        // text, "nowPlayingStill<name>" for each run of it that stays still,
        // "nowPlayingIncoming<name>" for the rest of it, which fades in, and
        // "nowPlayingIncoming<name>Window" for each piece of that,
        // "nowPlayingOutgoing<name>Layer" for each line before, and
        // "nowPlayingOutgoing<name>Part" for each piece of one, whose text
        // is "nowPlayingOutgoing<name>".
        required property string name
        property real pointSize: root.uiPalette.basePointSize
        property int weight: Font.Normal
        // Tabular figures where `_stillRuns` counts on them.
        readonly property var features: root.tabularLines[line.lineKey] === true
            ? root.uiPalette.tabularFigures : ({})
        property color color: root.uiPalette.text
        // Whether the line keeps its row when a track has no text for it,
        // as under the wide cover; otherwise it then takes no room.
        property bool keepsRow: false
        // Whether the line keeps the album's year whole after the album
        // (see LineText).
        property bool keepsTail: false
        readonly property bool collapsed: line.text.length === 0
        // More than one where the line wraps rather than be cut off: it
        // then fades whole, as no part of it stays in place for sure.
        property int maximumLineCount: 1
        // Whether a word wider than the line breaks with a hyphen rather
        // than anywhere (see _laidOut).
        property bool breaksLongWords: false
        // The height of the box each text of the line is centred in, or 0
        // for its top: the large-font header's title is centred on the
        // cover.
        property real seatHeight: 0
        readonly property bool wraps: line.maximumLineCount > 1
        readonly property bool truncated: stillText.elided
        // The lines the playing track's text takes.
        readonly property int textLineCount: stillText.lineCount

        readonly property var before: root.outgoing !== null
            ? (root.outgoing.lines[line.lineKey] || null) : null
        readonly property bool fading: line.before !== null && root.handover < 1
        readonly property var layers: line.fading ? line.before.layers : []
        // Every text the line shows, the playing track's last.
        readonly property var texts: line.layers.map(layer => layer.text).concat([line.text])
        // The playing track's text where it does not stay still, fading in.
        readonly property real incomingOpacity: line.fading ? root._lineInOpacity(line.before) : 1
        // How much of their opacity the texts before keep.
        readonly property real outgoingLevel: line.fading ? root._lineOutLevel(line.before) : 0
        // Where the line stays still while it fades, [left, right] for each
        // run of `still`: from the middle of the gap before it to the middle
        // of the gap after it, where no text has a glyph. A run whose gap
        // after it the ellipsis takes in one of the texts stays still to the
        // end of the line when all are cut off within it, alike, and fades
        // otherwise, as does all past it.
        readonly property var stillWindows: {
            if (!line.fading || line.wraps)
                return []
            const windows = []
            for (const run of line.before.still) {
                const start = run[0] === 0 ? 0 : run[0] - 1
                if (start > 0 && !line.texts.every(text => line._keeps(text, start)))
                    break
                const left = start === 0 ? 0 : line._edgeAfter(line.text.slice(0, start))
                let right = line.width
                if (!line.texts.every(text => text.length === run[1])) {
                    if (line.texts.every(text => line._keeps(text, run[1]))) {
                        right = Math.min(line.width, line._edgeAfter(line.text.slice(0, run[1])))
                    } else {
                        const elidedWithin = metrics.advanceWidth(line.text.slice(0, run[1]))
                            + metrics.advanceWidth("…") > line.width
                        if (!elidedWithin || !line.texts.every(text => line._cutOff(text)))
                            break
                    }
                }
                if (right > left)
                    windows.push([left, right])
                if (right >= line.width)
                    break
            }
            return windows
        }
        // Where the line changes while it fades: all but stillWindows.
        readonly property var changingWindows: {
            if (!line.fading)
                return []
            const windows = []
            let from = 0
            for (const still of line.stillWindows) {
                if (still[0] > from)
                    windows.push([from, still[0]])
                from = Math.max(from, still[1])
            }
            if (from < line.width)
                windows.push([from, line.width])
            return windows
        }

        // What `layer` draws, where the line changes: each of its parts
        // that shows, {x, width, from}, cut where the line stays still.
        function _pieces(layer) {
            const pieces = []
            const parts = layer.parts
            if (line.wraps) {
                // One piece: its parts are laid out on one line.
                const from = root._layerFrom(layer)
                return from > 0 ? [{ x: 0, width: line.width, from: from }] : []
            }
            for (let index = 0; index < parts.length; ++index) {
                if (parts[index].from <= 0)
                    continue
                const left = line._partEdge(layer.text, parts[index].at)
                const right = index + 1 < parts.length
                    ? line._partEdge(layer.text, parts[index + 1].at) : line.width
                for (const changing of line.changingWindows) {
                    const start = Math.max(left, changing[0])
                    const end = Math.min(right, changing[1])
                    if (start < end)
                        pieces.push({ x: start, width: end - start, from: parts[index].from })
                }
            }
            return pieces
        }

        // Where a part of `text` that starts at `at` starts on the line: in
        // the gap after the first `at` characters; at the end of the line
        // where `text` is elided, so the ellipsis is in one part.
        function _partEdge(text, at) {
            if (at === 0)
                return 0
            return line._keeps(text, at) ? Math.min(line.width, line._edgeAfter(text.slice(0, at))) : line.width
        }

        // The middle of the space after `start`, a start of a text the line
        // shows, as that text lays it out.
        function _edgeAfter(start) {
            // Read the font, for the edges to follow it.
            metrics.font
            return (metrics.advanceWidth(start) + metrics.advanceWidth(start + " ")) / 2
        }

        function _cutOff(text) {
            metrics.font
            return metrics.advanceWidth(text) > line.width
        }

        // Whether `text`, elided to the line, still shows its first `at`
        // characters, the space after them and the glyph after that, so that
        // the gap after them is one with no ellipsis in it.
        function _keeps(text, at) {
            return !line._cutOff(text)
                || metrics.advanceWidth(text.slice(0, at + 2)) + metrics.advanceWidth("…") <= line.width
        }

        Layout.fillWidth: true
        // A line of the font's height; a track's empty line takes none,
        // rather than leave a blank line in the block, unless it keeps its
        // row.
        implicitHeight: line.collapsed ? (line.keepsRow ? rowProbe.implicitHeight : 0)
                                       : stillText.implicitHeight + stillText.tailLineHeight
        visible: !line.collapsed || line.keepsRow

        // A line of the font, for a row kept empty.
        Text {
            id: rowProbe
            visible: false
            text: "X"
            textFormat: Text.PlainText
            font: stillText.font
            Accessible.ignored: true
        }

        FontMetrics {
            id: metrics
            font: stillText.font
        }

        // The playing track's text, whole while nothing fades; while the
        // line fades it draws through stillWindows and changingWindows.
        Item {
            width: line.fading ? 0 : line.width
            height: line.height
            clip: line.fading

            LineText {
                id: stillText
                objectName: "nowPlaying" + line.name
                owner: line
                wholeText: line.text
                Accessible.role: Accessible.StaticText
                Accessible.name: stillText.shownText
                Accessible.ignored: stillText.shownText.length === 0
            }
        }

        // What stays still: the playing track's text in stillWindows.
        Repeater {
            model: line.stillWindows

            delegate: Item {
                id: stillWindow

                required property var modelData

                objectName: "nowPlayingStill" + line.name
                x: stillWindow.modelData[0]
                width: stillWindow.modelData[1] - stillWindow.modelData[0]
                height: line.height
                clip: true

                LineText {
                    x: -stillWindow.x
                    owner: line
                    wholeText: line.text
                    Accessible.ignored: true
                }
            }
        }

        // What changes: the lines before, fading out part by part, the
        // oldest at the bottom, and over them the playing track's text in
        // changingWindows, fading in as one.
        Repeater {
            model: line.layers

            delegate: Item {
                id: lineBefore

                required property var modelData
                readonly property string text: lineBefore.modelData.text

                objectName: "nowPlayingOutgoing" + line.name + "Layer"
                width: line.width
                height: line.height

                Repeater {
                    model: line._pieces(lineBefore.modelData)

                    delegate: Item {
                        id: part

                        required property var modelData

                        objectName: "nowPlayingOutgoing" + line.name + "Part"
                        x: part.modelData.x
                        width: part.modelData.width
                        height: line.height
                        clip: true
                        opacity: part.modelData.from * line.outgoingLevel

                        LineText {
                            objectName: "nowPlayingOutgoing" + line.name
                            x: -part.x
                            owner: line
                            wholeText: lineBefore.text
                            Accessible.ignored: true
                        }
                    }
                }
            }
        }
        Item {
            objectName: "nowPlayingIncoming" + line.name
            z: 1
            width: line.width
            height: line.height
            visible: line.fading
            opacity: line.incomingOpacity

            Repeater {
                model: line.changingWindows

                delegate: Item {
                    id: changingWindow

                    required property var modelData

                    objectName: "nowPlayingIncoming" + line.name + "Window"
                    x: changingWindow.modelData[0]
                    width: changingWindow.modelData[1] - changingWindow.modelData[0]
                    height: line.height
                    clip: true

                    LineText {
                        x: -changingWindow.x
                        owner: line
                        wholeText: line.text
                        Accessible.ignored: true
                    }
                }
            }
        }
    }

    // How much of the cover's place shows: opening, from the compact row's
    // height to the square, in the wide layout all the move opens but the
    // lines' room (wideTextRoom); closing, in the wide layout all the seam
    // leaves but that room, and in the narrow ones what it showed, until
    // the seam comes up to its foot, which then goes with it, so no band of
    // nothing trails the cover.
    readonly property real coverBand: {
        if (root.presence >= 1)
            return root.coverSize
        const from = root.moveFrom
        const band = from.band >= 0 ? Math.min(root.coverSize, from.band)
                                    : Math.min(root.coverSize, root.idleRowHeight)
        if (from.closing)
            return Math.min(band, Math.max(0, root.moveSeam - root.coverInset
                                              - (root.wideRoom ? root.wideTextRoom : 0)))
        if (!root.wideRoom)
            return band + (root.coverSize - band) * root.openProgress
        return Math.min(root.coverSize, Math.max(band, root.moveSeam - root.wideTextRoom))
    }
    // The cover's place: the square, or, while the track's layout grows
    // from the compact row or shrinks back into it, a band of it from the
    // row's height up, which uncovers the cover from its top: the cover
    // lies whole, at its full size, at the place's top left all along, so
    // the move reveals it rather than scale it (a square in the middle of
    // the band, growing, read as a zoom).
    Item {
        id: coverArea
        objectName: "nowPlayingCoverArea"
        x: root.coverInset
        y: root.coverInset
        width: root.coverSize
        height: root._whileGrowing(root.coverBand)
        visible: root.presence > 0 && root.coverSize > 0
        clip: coverArea.height < root.coverSize

        Components.AlbumArtTile {
            id: cover
            objectName: "nowPlayingCover"
            x: 0
            y: 0
            width: root.coverSize
            height: root.coverSize
            layoutSize: root.coverSize
            opacity: root.coverOpacity
            uiBridge: root.uiBridge
            // While the panel shrinks into the compact row, the track that
            // went, its cover or stand-in, and nothing to open or focus.
            idle: root.idle && root.presence <= 0
            enabled: !root.idle
            coverPath: root.shownCoverPath
            // The size it loads its image at, whatever the place's size.
            coverSourceSize: Math.max(256, root.width)
            // No cover: the stand-in, its mark on the plot's floor, with a
            // 1 px edge in both themes on the sides where it meets the
            // sidebar's view colour (standInEdgeSides), so the dark tile
            // reads as a cover rather than a hole in the sidebar and its
            // sides do not dissolve into a dark one (the floor is ~1.06:1
            // on it).
            standInColor: root.uiPalette.plotFloor
            standInEdgeColor: root.uiPalette.plotFloorEdge
            standInEdgeSides: root.standInEdgeSides
            // Nothing playing: the placeholder, on the sidebar.
            placeholderColor: root.placeholderColor
            hoverVeilColor: root.uiPalette.hover
            focusRingColor: root.uiPalette.focusRing
            focusCasingColor: root.uiPalette.view
            // The sidebar's, which shows around the cover.
            backdropColor: root.uiPalette.view
            replaceFromItunesAction: root.replaceFromItunesAction
            currentTrackItunesArtworkDisabledReason: root.currentTrackItunesArtworkDisabledReason
            openAlbumArtViewer: root.openAlbumArtViewer
            // The covers before, over this one while they fade out.
            outgoingSlots: root.maxOutgoingLayers
            outgoingCovers: root.outgoing !== null ? root.outgoing.covers : []
            outgoingLevel: root.coversOutLevel
        }
    }

    // Nothing loaded: Ferrous's mark and "Nothing playing", in one row.
    Item {
        id: idleRow
        objectName: "nowPlayingIdleRow"
        z: 1
        x: 0
        y: 0
        width: root.width
        height: root.idleRowHeight
        opacity: root.idleRowOpacity
        visible: idleRow.opacity > 0

        Kirigami.Icon {
            objectName: "nowPlayingIdleGlyph"
            x: root.textInset
            anchors.verticalCenter: parent.verticalCenter
            width: root.idleGlyphSize
            height: width
            // Ferrous's mark, the stand-in's traces drawn for its size,
            // rather than a stock disc, which read as noise at this size.
            source: root.idleGlyphSource
            roundToIconSize: false
            isMask: true
            color: root.uiPalette.textSecondary
        }

        Text {
            objectName: "nowPlayingIdleLabel"
            x: root.textInset + root.idleGlyphSize + root.idleGlyphGap
            width: Math.max(0, root.width - x - root.textInset)
            anchors.verticalCenter: parent.verticalCenter
            text: root.idleText
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font: root.uiPalette.baseFont
            color: root.uiPalette.textSecondary
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    // The playing track's lines, each of which fades on its own over a
    // track change.
    Item {
        id: liveText
        objectName: "nowPlayingLines"

        readonly property bool truncated: root.wideRoom
            ? (wideTitle.truncated || wideArtistAlbum.truncated || wideDetails.truncated)
            : (narrowTitle.truncated || narrowArtist.truncated
                || narrowAlbum.truncated || narrowDetails.truncated)
        readonly property bool hovered: wideHover.hovered || narrowHover.hovered
            || albumHover.hovered || detailsHover.hovered
        readonly property Item wideItem: wideColumn
        readonly property Item narrowItem: narrowColumn

        x: 0
        y: 0
        width: root.width
        height: root.height

        // Wide: the text block under the cover.
        ColumnLayout {
            id: wideColumn
            objectName: "nowPlayingWideText"
            visible: root.wideRoom && (!root.idle || root.holdsTrack)
            x: root.textInset
            // Under the cover's place, with which it moves while that grows.
            y: root._whileGrowing(root.footTop + root.wideTextTop)
            width: Math.max(0, root.width - 2 * root.textInset)
            spacing: 2

            HoverHandler {
                id: wideHover
            }

            FadingLine {
                id: wideTitle
                lineKey: "title"
                name: "Title"
                roomNeeded: root.wideTextTop + wideTitleProbe.implicitHeight
                text: root.shownLines.title
                pointSize: root.wideTitlePointSize
                weight: Font.DemiBold
                keepsRow: true
            }
            FadingLine {
                id: wideArtistAlbum
                lineKey: "artistAlbum"
                name: "ArtistAlbum"
                roomNeeded: root.wideTextTop + wideTitleProbe.implicitHeight
                    + wideColumn.spacing + wideLineProbe.implicitHeight
                text: root.shownLines.artistAlbum
                keepsRow: true
            }
            FadingLine {
                id: wideDetails
                lineKey: "details"
                name: "Details"
                roomNeeded: root.wideTextTop + root.wideLinesHeight
                text: root.shownLines.details
                pointSize: root.uiPalette.smallPointSize
                color: root.uiPalette.textTertiary
                keepsRow: true
            }
        }

        // Narrow: four short lines beside the cover, or under it when the
        // room beside it is too small for them. A Column rather than a
        // layout: under the cover the title and the artist wrap, so their
        // height follows their width, which Qt Quick Layouts do not follow.
        Column {
            id: narrowColumn
            objectName: "nowPlayingNarrowText"
            visible: !root.wideRoom && (!root.idle || root.holdsTrack)
            // Beside the cover or under it; in the large-font header, the
            // title beside it and the rest under it.
            x: root.narrowStacked ? root.textInset
                : root.coverInset + root.narrowLayoutCoverSize + root.titleGapBeside
            // Under the cover's place, with which it moves while that grows;
            // beside it, from under the compact row up into place, so its
            // text does not show where the row's does.
            y: root._whileGrowing(root.narrowStacked
                ? root.footTop + root.textInset
                : root.besideTextTop + Math.round(root.lineShift * root.idleRowHeight))
            width: Math.max(0, root.width - (root.largeHeader ? root.headerTitleEnd : root.textInset)
                                   - narrowColumn.x)
            spacing: root.narrowStacked || root.largeHeader ? root.stackedLineGap : 0

            HoverHandler {
                id: narrowHover
            }

            FadingLine {
                id: narrowTitle
                lineKey: "title"
                name: "NarrowTitle"
                text: root.shownLines.title
                // One size for every title of the layout (headerTitleSmall).
                pointSize: root.narrowTitlePointSize
                weight: Font.DemiBold
                maximumLineCount: 2
                breaksLongWords: true
                // In the large-font header, centred on the cover, on one
                // line or two, so the header reads as one block (on the
                // cover's foot it hung, with some 60 px empty over it; at
                // its top it left some 54 px empty under it).
                seatHeight: root.largeHeader ? root.headerCoverSize : 0
                width: narrowColumn.width
                // In the large-font header, the cover's height and footGap
                // with the column's spacing under it, so the lines under the
                // cover start there whatever the title takes. (A spacer for
                // it would be 0 px under a title on two lines, and a Column
                // passes over an item of no height, and its spacing.)
                height: root.largeHeader
                    ? Math.max(implicitHeight, root.headerCoverSize + root.footGap - narrowColumn.spacing)
                    : implicitHeight
            }
            // Beside the cover, narrowTitleGap between the title and the
            // artist; under it the column's spacing.
            Item {
                id: titleGap
                visible: !root.narrowStacked && !root.largeHeader
                    && (narrowArtist.visible || narrowAlbum.visible || narrowDetails.visible)
                width: 1
                height: root.narrowTitleGap
            }
            FadingLine {
                id: narrowArtist
                lineKey: "artist"
                name: "NarrowArtist"
                text: root.shownLines.artist
                pointSize: root.uiPalette.smallPointSize
                // One line, elided, with the whole of it in the tooltip.
                // Under the cover the title and the artist share their room:
                // under a title on one line, the artist may take two (on
                // one line, a long artist was cut off with a line free).
                maximumLineCount: root.narrowStacked && narrowTitle.textLineCount < 2 ? 2 : 1
                // Under the large-font header, at the panel's inset.
                x: root.largeHeader ? root.textInset - narrowColumn.x : 0
                width: root.largeHeader ? root.footLineWidth : narrowColumn.width
                keepsRow: root.largeHeader
            }
            FadingLine {
                id: narrowAlbum
                lineKey: "narrowAlbum"
                name: "NarrowAlbum"
                text: root.shownLines.narrowAlbum
                keepsTail: true
                pointSize: root.uiPalette.smallPointSize
                // In every narrow layout a step below the artist and above
                // the details (at the artist's size, weight and colour,
                // with no gap, the two read as one name that wrapped).
                color: root.uiPalette.textSecondary
                // Beside the cover in the compact layout, the year goes
                // onto a second line with the album's end where it needs
                // to; under the cover too, in the line a title on one line
                // leaves when the artist takes one; elsewhere one line, the
                // album cut off before its year.
                maximumLineCount: root.narrowLinesWrap ? 2 : 1
                // Under the large-font header, at the panel's inset, its row
                // kept for every track, so nothing under it moves.
                x: root.largeHeader ? root.textInset - narrowColumn.x : 0
                width: root.largeHeader ? root.footLineWidth : narrowColumn.width
                keepsRow: root.largeHeader

                HoverHandler {
                    id: albumHover
                }
            }
            FadingLine {
                id: narrowDetails
                lineKey: "narrowDetails"
                name: "NarrowDetails"
                text: root.shownLines.narrowDetails
                pointSize: root.uiPalette.smallPointSize
                color: root.uiPalette.textTertiary
                x: root.largeHeader ? root.textInset - narrowColumn.x : 0
                width: root.largeHeader ? root.footLineWidth : narrowColumn.width
                keepsRow: root.largeHeader

                HoverHandler {
                    id: detailsHover
                }
            }
        }
    }

    Rectangle {
        objectName: "nowPlayingSeam"
        x: 0
        y: root.height - root.seamHeight
        width: root.width
        height: root.seamHeight
        color: root.uiPalette.seam
    }

    // The whole text of the lines, when one of them is cut off, or the
    // details are without a part of them.
    readonly property bool textTruncated: liveText.truncated
        || (root.wideRoom ? root.shownDetailsText !== root.detailsText
                          : root.shownNarrowDetailsText !== root.narrowDetailsText)

    Components.PlainToolTip {
        parent: root.wideRoom ? liveText.wideItem : liveText.narrowItem
        visible: liveText.hovered && root.textTruncated
        delay: Kirigami.Units.toolTipDelay
        message: root.wideRoom
            ? root._joined([root.titleText, root.artistAlbumText, root.folderText, root.detailsText,
                            root.fileFactsText], "\n")
            : root._joined([root.titleText, root.artistText, root.folderText, root.narrowAlbumText,
                            root.narrowDetailsText, root.fileFactsText], "\n")
    }
}
