// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils

// The top of the sidebar: the playing track, on the view colour with no
// card, and a seam at its foot, where the library starts.
//
// - Wide: the cover edge to edge, then the title, "Artist — Album" and
//   "Year · Genre · Track n" under it, inset 12 px, a line each, in a
//   block that with the cover is a whole number of the list's rows high
//   (see wideTextTop), then the seam. The block is one height for every
//   track (#373 H14 G1: an untagged track's, a tagged one's and one with a
//   long album's were 56, 82 and 108 px, so a track change moved the
//   library by up to 26 px): each line is elided where it does not fit,
//   with the whole text in the tooltip, and a line the track has no text
//   for keeps its row, empty.
// - Narrow (a sidebar under narrowWidth): a 96 px cover at the panel's top
//   left corner, as the wide one is, at every font size (#373 H14 G9: in
//   a wide sidebar it was in 12 px, at the top of the title's capitals),
//   with four short lines beside it, 12 px after it and 10 px down, or
//   under it when there is no room beside it or they would run well past
//   it, then 12 px before the seam. Under it, the
//   title and the artist wrap onto a second line rather than be cut off,
//   and the lines are 4 px apart. Beside the cover in a wide sidebar (the
//   compact layout) the block keeps six lines' room; with a font too
//   large for four lines beside the 96 px cover, as 24 pt, it is a header
//   row instead, the cover with the title and the artist beside it, and
//   the album and the details under it on the whole width (see
//   compactHeader). A sidebar too short for the wide layout
//   to leave the library about six rows, as with a large font or a short
//   window, takes it too (#373 H6), with the lines beside the cover
//   however far past it they run, as its room beside it is wide (#373 H10).
// - Nothing loaded, wide or narrow: one compact row of the list's row
//   height, a 16 px disc mark at the 12 px inset and "Nothing playing"
//   where a library row has its text, both in the secondary colour; the
//   library gets the height (#373 H7: the empty square took 360 px). The
//   first track grows the cover's place from the row to its full size
//   over moveDuration, InOutQuad, uncovering the cover, which lies at its
//   full size in place all along (#373 H14 G5: a cover scaled up from a
//   square in the middle of the band read as a zoom, with empty flanks),
//   fading in as the row goes, by the move's time (see idleOut), the
//   lines and the library moving down
//   with its foot, so nothing jumps (#373 H5); the lines fade in once the
//   move is nearly done, so no text slides over the moving rows (#373 H12
//   Z7). Going back to nothing
//   loaded, as when the queue is cleared, shrinks it back. With
//   animations off, both are at once.
// - A track without a cover keeps the cover's place, wide or narrow, with
//   a stand-in in it (#373 H11; AlbumArtTile): the plot's floor colour,
//   with the album at its top left, at the lines' inset, as a display
//   title; never the artist, nor what the lines would say alone, so
//   without an album, or with one named as the track, the plain colour
//   (#373 H12 Z1: the album and artist at its foot, or the file's name,
//   said again what the lines under it say). So the panel
//   changes its geometry only between nothing loaded and a track (#373
//   H9 to H11: a track without a cover had a layout of its own, a mark
//   beside the title and no cover's place, and a track change between one
//   and the other moved the library some 360 to 400 px). Between a track
//   with a cover and one without, the cover and the stand-in cross-fade
//   in place, as two covers do; the lines and the library stay put.
// - Narrow, a line a track has no text for, as for a file without tags,
//   takes no room (#373 H6): the lines under it move up, within the block.
//
// The title is over the rest: DemiBold, and a Kirigami heading's size,
// level 1 under the wide cover and level 2 beside the narrow one; the
// artist and the album are in the text colour, and the details, one line
// in one colour, in the tertiary one. Details that do not fit leave out
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
    // window, the panel takes the narrow one (#373 H6: at 24 pt in a
    // 1000 px high window the library showed three rows). The wide
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
    // The compact layout's cover (see compactLayout): the narrow 96 px,
    // and with a font too large for the four lines beside it
    // (compactHeader), as high as the lines beside it at their most, two
    // of the title, the gap and one of the artist, from the fonts alone,
    // so that it spans them (#373 H16 I1: a 132 px cover beside six lines
    // left some 160 px empty under it).
    readonly property real compactCoverSize: root.largeFont
        ? Math.max(root.narrowCoverSize, Math.round(narrowTitleProbe.implicitHeight
                                                    + root.narrowTitleGap + narrowLineProbe.implicitHeight))
        : root.narrowCoverSize
    // The cover of the narrow layout: the compact one's beside the compact
    // lines, the 96 px one otherwise.
    readonly property real narrowLayoutCoverSize: root.compactLayout ? root.compactCoverSize
                                                                     : root.narrowCoverSize

    readonly property bool hasTrack: {
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
    // InOutQuad, which eases in rather than leaping off. Under the wide
    // cover the library moves 350 to 420 px: over longDuration, OutCubic,
    // its first frames jumped some 100 px, four of its rows, and read as a
    // cross-fade rather than a slide (#373 H10); over veryLongDuration,
    // OutCubic, it still started at some 3,100 px/s (#373 H12 Z7); over
    // wideMoveDuration, InOutQuad, it peaks at some 1,850 px/s, mid-way.
    // The narrow cover's 100 px or so take longDuration. None with
    // animations off.
    readonly property int wideMoveDuration: Math.round(Kirigami.Units.veryLongDuration * 9 / 8)
    readonly property int moveDuration: Kirigami.Units.longDuration <= 0 ? 0
        : (root.wideRoom ? root.wideMoveDuration : Kirigami.Units.longDuration)
    readonly property int moveEasing: Easing.InOutQuad
    // The lines come in after the move, not with it (#373 H12 Z7: they
    // slid over the moving rows for some 190 ms): at 0 until the move is
    // linesFromPresence of the way (#373 H14 G5: from 80 % they still
    // arrived with the move), then fading in over shortDuration, OutCubic,
    // or what is left of the move where that is shorter, so they are
    // whole when it ends. Read from the move's progress, so a move that
    // turns back, or none, as with animations off, takes them with it.
    readonly property real linesFromPresence: 0.9
    readonly property real linesOpacity: {
        if (root.presence >= 1)
            return 1
        if (root.presence <= 0 || root.moveDuration <= 0)
            return 0
        const start = root._moveTime(root.linesFromPresence) * root.moveDuration
        const length = Math.max(1, Math.min(Kirigami.Units.shortDuration, root.moveDuration - start))
        return root._eased((root._moveTime(root.presence) * root.moveDuration - start) / length)
    }
    // The compact row of nothing loaded and the cover hand over by the
    // move's time, not by how far it has come, so they take the same
    // milliseconds over any move: the row goes over idleOut, linear, the
    // cover comes in over coverIn, OutCubic, whole for the rest; they
    // overlap only where each is near half, so the band never shows
    // neither, nor "Nothing playing" across the art for long (#373 H15 H5:
    // the row's text, at 0.8 to 0.2, was drawn across the incoming art;
    // H16 I2: on InOutQuad's slow start the row was gone by 12 % of the
    // way and the cover still faint, so the band was empty for some 60 to
    // 140 ms). As fractions of shortDuration, [start, end]: the row from
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
    // InOutQuad's inverse.
    function _moveTime(presence) {
        const p = Math.min(1, Math.max(0, presence))
        return p < 0.5 ? Math.sqrt(p / 2) : 1 - Math.sqrt((1 - p) / 2)
    }
    readonly property string idleText: I18n.i18nc("@info", "Nothing playing")

    // The compact row of nothing loaded: a list row's height, with the
    // library's glyph size and gap, so its text starts where a library
    // row's does.
    readonly property real idleRowHeight: root.uiPalette.rowHeight
    readonly property int idleGlyphSize: root.uiPalette.toolIconSize
    readonly property int idleGlyphGap: 6
    // The panel's height with nothing loaded: the row and the seam.
    readonly property real restHeight: root.idleRowHeight + root.seamHeight
    // The side of the cover's square, at the panel's top left: the
    // sidebar's width in the wide layout, 96 px in the narrow one, or the
    // compact layout's (compactCoverSize), whether the track has a cover
    // or its stand-in.
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
    readonly property NumberAnimation presenceAnimation: NumberAnimation {
        objectName: "nowPlayingPresenceAnimation"
        target: root
        property: "presence"
        easing.type: root.moveEasing
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
    // not fit, "A Remarkabl… (2024)" (#373 H14 G2: at 24 pt the year was
    // lost to the ellipsis). The parts are read from the line's own
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
    readonly property string narrowDetailsText: root._joined([root.genreText, root.trackNumberText],
                                                             root.lineSeparators.narrowDetails)
    // The details as the lines show them: all of them where the line has
    // room, and otherwise without what matters least, rather than cut off
    // (#373 H11: at 24 pt, "Classical · Track 2 o…"): first the album's
    // track total, then the genre, then the year. The whole of them is in
    // the tooltip.
    function _fittingDetails(year, separator, width) {
        const candidates = [
            [year, root.genreText, root.trackNumberText],
            [year, root.genreText, root.trackNumberShortText],
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
        root.yearText, root.lineSeparators.details, Math.max(0, root.width - 2 * root.textInset))
    readonly property string shownNarrowDetailsText: root._fittingDetails(
        "", root.lineSeparators.narrowDetails, root.compactHeader ? root.footLineWidth : narrowColumn.width)

    // The lines the panel shows, wide and narrow.
    readonly property var trackLines: ({
        title: root.titleText,
        artistAlbum: root.artistAlbumText,
        details: root.shownDetailsText,
        artist: root.artistText,
        narrowAlbum: root.narrowAlbumText,
        narrowDetails: root.shownNarrowDetailsText
    })

    // A track change fades (#373 Motion), line by line: a line that differs
    // for the next track cross-fades, in place, the one before going out
    // over the first 40 % of the fade as the next track's comes in over all
    // of it (see _linesOutLevel), so the line never drops below half of its
    // ink (#373 H6: out, then in, left each line under half for up to 50
    // ms) and the two texts hardly show together (#373 H15 H3); where
    // only a word or a number of it changes, its fields, and the words of a
    // field in which only a number changes, that are the same for both and
    // do not move stay still (#537), and otherwise it fades whole (see
    // `_fadesWhole`, #373 H2); a line that is the same stays still. The next
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
    readonly property string trackKey: root.hasTrack
        ? root._trimmed(root.uiBridge.currentTrackPath) : ""
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
    // playing track alone shows. Every layer eases on it (see _linesOutLevel).
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
    // At `at` of the fade, how much of their opacity the lines before keep
    // and how far the playing track's lines have come in; and how much of
    // their opacity the covers before keep, going out over all of it. The
    // lines before go out over the first linesOutSpan of it, OutCubic, 80
    // ms of a 200 ms fade, as the playing track's come in over all of it,
    // OutCubic, so the two texts are both over 0.3 for some 4 ms of it
    // rather than 50 (#373 H15 H3: two titles read superimposed), and
    // between them a line keeps over half of a text's ink in every frame,
    // so it does not blink (#373 H6). The incoming lines start with the
    // fade: held back any longer, as to 60 ms, the line would drop under
    // half its ink for up to 70 ms, a blink.
    readonly property real linesOutSpan: 0.4
    function _linesOutLevel(at) {
        return 1 - root._eased(at / root.linesOutSpan)
    }
    function _linesInLevel(at) {
        return root._eased(at)
    }
    function _coversOutLevel(at) {
        return 1 - root._eased(at)
    }
    readonly property real linesOutLevel: root._linesOutLevel(root.handover)
    readonly property real linesInLevel: root._linesInLevel(root.handover)
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
    // all share.
    function _fadingLine(still, layers, incomingFrom, own) {
        return { still: still, layers: layers, incomingFrom: incomingFrom, own: own }
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
            * root._linesOutLevel(root.handover)
    }

    // How much of the playing track's text of `key` has still to come in.
    function _incomingLineRest(key) {
        const line = root._outgoingLine(key)
        return line === null ? 0 : line.incomingFrom * (1 - root._linesInLevel(root.handover))
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
        if (root.wrappingLines[key] === true)
            return []
        return root._stillRuns(before, after, root.lineSeparators[key] || "", root.tabularLines[key] === true)
    }

    // The lines that wrap onto a second line rather than be cut off, and
    // so fade whole: in the narrow layout the title (#373 H11: beside the
    // cover at 24 pt a long title was cut off), under the narrow cover the
    // artist too, and under it and beside it in the compact layout the
    // album with its year (#373 H14 F7, G2: at 24 pt a long album kept a
    // fifth of its text), but under the header row (compactHeader), where
    // it has the whole width, one line, its year whole after it. Beside the
    // compact cover the artist takes one line, elided, with the whole of it
    // in the tooltip (#373 H15 H1). The album line fades whole wherever it
    // shows, as its year stands apart from it (_albumAndYear).
    readonly property var wrappingLines: root.wideRoom ? ({})
        : (root.narrowStacked ? ({ title: true, artist: true, narrowAlbum: true })
                              : ({ title: true, narrowAlbum: true }))
    readonly property bool narrowLinesWrap: root.narrowStacked || (root.compactLayout && !root.compactHeader)
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
        const level = root._linesOutLevel(root.handover)
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
            trackKey: root.trackKey,
            album: root.albumText,
            coverPath: root.coverPath,
            lines: root.trackLines
        }
        const was = root.shown
        root.shown = next
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
    // Kirigami.Heading's size, 13.5 pt with the default 10 pt font (#373
    // H11: a step up from level 2, to stand to the 360 px cover), and in
    // the narrow layout a level 2's, 12 pt. It follows the font.
    readonly property real wideTitlePointSize: root.uiPalette.headingPointSize(1)
    readonly property real titlePointSize: root.uiPalette.headingPointSize(2)

    // The wide text block, under the cover, is on the queue's row grid
    // (#373 H11): with the cover it is a whole number of the list's rows
    // high, UiPalette's rowHeight, which the queue's rows are too, so that
    // the seam at its foot and the hairline under the library's header
    // fall on the lines of the queue's rows; with the default font and the
    // 360 px sidebar the block is 82 px, and the header's hairline
    // continues the seam between the queue and the visualization. The
    // lines start `wideTextTop` under the cover, whatever the track, and
    // what the grid leaves over is under them, at least `wideLeastGap`
    // before the seam, which sets the panel and the library apart as
    // regions of one sheet (#373 H4: 28 px there left a dead band).
    readonly property int wideTextTop: 2 * Kirigami.Units.smallSpacing
    readonly property int wideLeastGap: 2 * Kirigami.Units.smallSpacing
    function _wideBlockHeight(linesHeight) {
        const row = root.uiPalette.rowHeight
        const least = root.width + root.wideTextTop + linesHeight + root.wideLeastGap
        return row > 0 ? Math.ceil(least / row) * row - root.width : least - root.width
    }
    // The 1 px seam at the panel's foot, where the library starts.
    readonly property int seamHeight: 1
    // The top of the lines beside the narrow cover; beside the compact
    // header's cover, at its top (besideTextTop).
    readonly property int narrowTextTop: 10
    readonly property int besideTextTop: root.compactHeader ? 0 : root.narrowTextTop
    // Where the line under the cover starts down: under the cover's
    // place, and while the track's layout grows from the compact
    // row, or shrinks into it, never above the row's foot, so the lines do
    // not show where the row's text does.
    readonly property real footTop: Math.max(coverArea.height,
        Math.round((1 - root.presence) * root.idleRowHeight))
    // The least width of the lines beside the narrow cover: about 12
    // characters of the font.
    readonly property real narrowLineMinimum: 12 * baseMetrics.averageCharacterWidth
    // How tall the four lines are beside the narrow cover, from its top.
    readonly property real narrowLinesHeight: root.narrowTextTop + narrowTitleMetrics.height
        + root.narrowTitleGap + 3 * narrowLineMetrics.height
    // Beside the narrow cover, the gap between the title and the artist
    // under it, which sets the title apart from the three small lines that
    // are one group (#373 H12: 2 px, the same as between those, ran them
    // together). The block stays the cover's height and the grid's.
    readonly property int narrowTitleGap: 4
    // The lines go under the cover, on the whole width, where the room
    // beside it is under narrowLineMinimum, or where, in a narrow sidebar,
    // they would run well past the cover, as with a large font: there the
    // room beside it would still show only a dozen characters of each,
    // under the cover they show more, and the title and the artist wrap
    // rather than be cut off (#373 H5). A sidebar wide enough for the wide
    // layout that takes this one for its height alone, as with a large
    // font in a 1000 px high window, keeps the lines beside the cover,
    // past its foot if they need it: the room beside it is wide enough
    // there, and under it the cover stood alone on a row, with its width
    // empty beside it (#373 H10).
    // The room beside the cover is that of the compact one in a sidebar
    // wide enough for the wide layout (compactLayout).
    readonly property bool narrowStacked: root.width - (root.narrow ? root.narrowCoverSize : root.compactCoverSize)
            - 2 * root.textInset < root.narrowLineMinimum
        || (root.narrow && root.narrowLinesHeight > 1.25 * root.narrowCoverSize)
    // The compact layout: the lines beside the cover in a sidebar wide
    // enough for the wide layout, which takes this one for its height
    // alone, as with a large font. There the album with its year takes two
    // lines where it needs them, as the title does (#373 H14 G2: at 24 pt
    // a long album was cut off on one line, its year lost), the artist one,
    // elided, and the block keeps the room of them all, six lines: two
    // each for the title and the album, one each for the artist and the
    // details, whatever the track, so a track change does not move the
    // library (#373 H15 H1: two for the artist too left some 145 px blank
    // under a common track).
    readonly property bool compactLayout: !root.wideRoom && !root.narrow && !root.narrowStacked
    readonly property real compactLinesHeight: root.narrowTextTop + narrowTitleProbe.implicitHeight
        + root.narrowTitleGap + narrowTwoLinesProbe.implicitHeight + 2 * narrowLineProbe.implicitHeight
    // A font too large for the four lines beside the 96 px cover, as 24 pt:
    // from the fonts alone, so a track does not switch it.
    readonly property bool largeFont: root.narrowLinesHeight > root.narrowCoverSize
    // The compact layout with a large font: a header row and lines on the
    // whole width (#373 H16 I1: beside a 132 px cover, six lines left the
    // cover with some 160 px empty under it, the year on a line of its own
    // and the artist cut off). The cover at the top left, the title, on up
    // to two lines, and the artist, on one, elided, or two under a title
    // on one, beside it, 12 px after it, from its top, the cover as high as
    // they are at their most (compactCoverSize); then, footGap under the
    // cover, at the panel's inset and on its whole width, "Album (Year)" on
    // one line, the album cut off before its year, in textSecondary, and
    // the details, on one line. The block is
    // one height for every track (compactHeaderHeight): a line a track has
    // no text for keeps its row, so a track with a title alone reads as a
    // header row, and a track change does not move the library.
    readonly property bool compactHeader: root.compactLayout && root.largeFont
    readonly property int footGap: 8
    readonly property real footLineWidth: Math.max(0, root.width - 2 * root.textInset)
    readonly property real compactHeaderHeight: root.compactCoverSize + root.footGap
        + 2 * narrowLineProbe.implicitHeight

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
    LineProbe {
        id: narrowTwoLinesProbe
        text: "X\nX"
        font.pointSize: root.uiPalette.smallPointSize
    }
    LineProbe {
        id: narrowLineProbe
        font.pointSize: root.uiPalette.smallPointSize
    }

    // The tile's own mark, which shows only while the tile grows from the
    // compact row before the cover is known, or shrinks into it: a third
    // of the tile, and never over 48 px, more with a larger font. Even, so
    // that it sits on whole pixels at the centre of the tile.
    readonly property int coverMarkSize: 2 * Math.round(Math.min(Math.round(cover.width / 3),
        root.uiPalette.scaledSize(Kirigami.Units.iconSizes.large)) / 2)
    // The tile with no cover in it: the sidebar's view colour, as
    // one surface with it (#373 H6: a lighter tile read as a broken image),
    // and its mark, the tertiary text colour mixed into the view, neutral,
    // at 3:1 on it, the least for a graphic to read (#373 H7: at 38% it was
    // 2:1 and faint), with a margin rendering cannot round away.
    readonly property color placeholderColor: root.uiPalette.view
    readonly property color placeholderMarkColor: root.uiPalette.readableOn(
        ColorUtils.mixColor(root.uiPalette.view, root.uiPalette.textTertiary, 0.38),
        [root.uiPalette.view], 3.1)

    // The playing track's block follows its text: in narrow, at least the
    // cover's height, and more when the lines need it.
    readonly property real narrowLayoutHeight: (root.narrowStacked
            ? root.narrowCoverSize + root.textInset + liveText.narrowHeight
            : root.compactHeader ? root.compactHeaderHeight
            : Math.max(root.narrowLayoutCoverSize, root.compactLayout ? root.compactLinesHeight
                                                                : root.narrowTextTop + liveText.narrowHeight))
        + root.textInset + root.seamHeight
    // The wide block's height: its three lines', whatever the track has
    // text for (#373 H14 G1), from the font alone.
    readonly property real wideLinesHeight: wideTitleProbe.implicitHeight + wideColumn.spacing
        + wideLineProbe.implicitHeight + wideColumn.spacing + wideDetailsProbe.implicitHeight
    readonly property real wideBlockHeight: root._wideBlockHeight(root.wideLinesHeight)
    readonly property real wideLayoutHeight: root.width + root.wideBlockHeight + root.seamHeight
    readonly property real layoutHeight: root.wideRoom ? root.wideLayoutHeight : root.narrowLayoutHeight
    // The compact row with nothing loaded, the block with a track, and
    // between them while one grows into the other.
    implicitHeight: root._whileGrowing(root.restHeight
        + ((root.idle ? root.heldLayoutHeight : root.layoutHeight) - root.restHeight) * root.presence)
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
        readonly property var parts: lineText.owner.keepsTail
            ? root._albumAndYear(lineText.wholeText) : [lineText.wholeText, ""]
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

        text: lineText.parts[0]
        width: owner.width
        elide: Text.ElideRight
        maximumLineCount: owner.maximumLineCount
        wrapMode: owner.maximumLineCount > 1 ? Text.Wrap : Text.NoWrap
        textFormat: Text.PlainText
        font.family: root.uiPalette.baseFont.family
        font.pointSize: owner.pointSize
        font.weight: owner.weight
        font.features: owner.features
        color: owner.color

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
        readonly property bool wraps: line.maximumLineCount > 1
        readonly property bool truncated: stillText.truncated
        // The lines the playing track's text takes.
        readonly property int textLineCount: stillText.lineCount

        readonly property var before: root.outgoing !== null
            ? (root.outgoing.lines[line.lineKey] || null) : null
        readonly property bool fading: line.before !== null && root.handover < 1
        readonly property var layers: line.fading ? line.before.layers : []
        // Every text the line shows, the playing track's last.
        readonly property var texts: line.layers.map(layer => layer.text).concat([line.text])
        // The playing track's text where it does not stay still, fading in.
        readonly property real incomingOpacity: line.fading
            ? 1 - line.before.incomingFrom * (1 - root.linesInLevel) : 1
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
        // rather than leave a blank line in the block (#373 H6), unless it
        // keeps its row.
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
                Accessible.name: wholeText
                Accessible.ignored: wholeText.length === 0
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
                        opacity: part.modelData.from * root.linesOutLevel

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

    // The cover's place: the square, or, while the track's layout grows
    // from the compact row or shrinks back into it, a band of it from the
    // row's height up, which uncovers the cover from its top: the cover
    // lies whole, at its full size, at the place's top left all along, so
    // the move reveals it rather than scale it (#373 H14 G5: H14 F5's
    // square in the middle of the band, growing, read as a zoom).
    Item {
        id: coverArea
        objectName: "nowPlayingCoverArea"
        x: 0
        y: 0
        width: root.coverSize
        height: root._whileGrowing(Math.min(root.coverSize, root.idleRowHeight)
            + Math.max(0, root.coverSize - root.idleRowHeight) * root.presence)
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
            idle: root.idle
            coverPath: root.coverPath
            // The size it loads its image at, whatever the place's size.
            coverSourceSize: Math.max(256, root.width)
            // No cover: the stand-in, its mark on the plot's floor, and in
            // a light theme an edge, so the dark tile reads as a cover
            // rather than a hole in the sidebar.
            standInColor: root.uiPalette.plotFloor
            standInEdgeColor: root.uiPalette.themeIsDark ? "transparent" : root.uiPalette.plotFloorEdge
            // Nothing playing: the placeholder, on the sidebar.
            placeholderColor: root.placeholderColor
            placeholderGlyphColor: root.placeholderMarkColor
            placeholderGlyphSize: root.coverMarkSize
            hoverVeilColor: root.uiPalette.hover
            focusRingColor: root.uiPalette.focusRing
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

    // Nothing loaded: the disc mark and "Nothing playing", in one row.
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
            source: "media-optical-audio-symbolic"
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

        readonly property real narrowHeight: narrowColumn.implicitHeight
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
        // After the move from the compact row, not with it.
        opacity: root.linesOpacity

        // Wide: the text block under the cover.
        ColumnLayout {
            id: wideColumn
            objectName: "nowPlayingWideText"
            visible: root.wideRoom && !root.idle
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
                text: root.trackLines.title
                pointSize: root.wideTitlePointSize
                weight: Font.DemiBold
                keepsRow: true
            }
            FadingLine {
                id: wideArtistAlbum
                lineKey: "artistAlbum"
                name: "ArtistAlbum"
                text: root.trackLines.artistAlbum
                keepsRow: true
            }
            FadingLine {
                id: wideDetails
                lineKey: "details"
                name: "Details"
                text: root.trackLines.details
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
            visible: !root.wideRoom && !root.idle
            // Beside the cover or under it.
            x: root.narrowStacked ? root.textInset : root.narrowLayoutCoverSize + root.textInset
            // Under the cover's place, with which it moves while that grows;
            // beside it, from under the compact row up into place, so its
            // text does not show where the row's does.
            y: root._whileGrowing(root.narrowStacked
                ? root.footTop + root.textInset
                : root.besideTextTop + Math.round((1 - root.presence) * root.idleRowHeight))
            width: Math.max(0, root.width - root.textInset - narrowColumn.x)
            spacing: root.narrowStacked ? root.stackedLineGap : 0

            HoverHandler {
                id: narrowHover
            }

            FadingLine {
                id: narrowTitle
                lineKey: "title"
                name: "NarrowTitle"
                text: root.trackLines.title
                pointSize: root.titlePointSize
                weight: Font.DemiBold
                maximumLineCount: 2
                width: narrowColumn.width
            }
            // Beside the cover, narrowTitleGap between the title and the
            // artist; under it the column's spacing.
            Item {
                id: titleGap
                visible: !root.narrowStacked
                    && (narrowArtist.visible || narrowAlbum.visible || narrowDetails.visible)
                width: 1
                height: root.narrowTitleGap
            }
            FadingLine {
                id: narrowArtist
                lineKey: "artist"
                name: "NarrowArtist"
                text: root.trackLines.artist
                pointSize: root.uiPalette.smallPointSize
                // Two lines under the cover; one beside it, elided, with
                // the whole of it in the tooltip. In the compact header the
                // title and the artist share the lines beside the cover:
                // under a title on one line, the artist may take two (#373
                // H18 C2: a long artist was cut off with a line free).
                maximumLineCount: root.narrowStacked
                    || (root.compactHeader && narrowTitle.textLineCount < 2) ? 2 : 1
                width: narrowColumn.width
                keepsRow: root.compactHeader
            }
            // In the compact header, what is left of the cover's height
            // and footGap under it, so the lines under it start there
            // whatever the title and the artist take.
            Item {
                objectName: "nowPlayingHeaderFoot"
                visible: root.compactHeader
                width: 1
                height: Math.max(0, root.compactCoverSize + root.footGap
                    - (narrowTitle.visible ? narrowTitle.height : 0)
                    - (titleGap.visible ? titleGap.height : 0)
                    - (narrowArtist.visible ? narrowArtist.height : 0))
            }
            FadingLine {
                id: narrowAlbum
                lineKey: "narrowAlbum"
                name: "NarrowAlbum"
                text: root.trackLines.narrowAlbum
                keepsTail: true
                pointSize: root.uiPalette.smallPointSize
                // Under the compact header, a step below the artist and
                // above the details, as the wide layout's lines are ranked
                // (#373 H18 C2).
                color: root.compactHeader ? root.uiPalette.textSecondary : root.uiPalette.text
                // Under the cover, and beside it in the compact layout, the
                // year goes onto a second line with the album's end; under
                // the compact header, on the whole width, one line.
                maximumLineCount: root.narrowLinesWrap ? 2 : 1
                // Under the compact header, at the panel's inset.
                x: root.compactHeader ? root.textInset - narrowColumn.x : 0
                width: root.compactHeader ? root.footLineWidth : narrowColumn.width
                keepsRow: root.compactHeader

                HoverHandler {
                    id: albumHover
                }
            }
            FadingLine {
                id: narrowDetails
                lineKey: "narrowDetails"
                name: "NarrowDetails"
                text: root.trackLines.narrowDetails
                pointSize: root.uiPalette.smallPointSize
                color: root.uiPalette.textTertiary
                x: root.compactHeader ? root.textInset - narrowColumn.x : 0
                width: root.compactHeader ? root.footLineWidth : narrowColumn.width
                keepsRow: root.compactHeader

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

    ToolTip {
        parent: root.wideRoom ? liveText.wideItem : liveText.narrowItem
        visible: liveText.hovered && root.textTruncated
        delay: Kirigami.Units.toolTipDelay
        text: root.wideRoom
            ? root._joined([root.titleText, root.artistAlbumText, root.detailsText], "\n")
            : root._joined([root.titleText, root.artistText, root.narrowAlbumText,
                            root.narrowDetailsText], "\n")
    }
}
