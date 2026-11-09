// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components
import "../logic/FormatUtils.js" as FormatUtils

// The top of the sidebar: the playing track, on the view colour with no
// card.
//
// - Wide: the cover edge to edge, then the title, "Artist — Album" and
//   "Year · Genre · Track n" under it, inset 12 px.
// - Narrow (a sidebar under narrowWidth): a 96 px cover with four short
//   lines beside it, then 12 px before the library.
// - Nothing loaded: one row with the album glyph and "Nothing playing".
//
// Lines that do not fit are elided, with the whole text in a tooltip.
Item {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer

    // The sidebar width under which the cover sits beside the text.
    readonly property real narrowWidth: 320
    readonly property bool narrow: root.width < root.narrowWidth
    readonly property int textInset: 12
    readonly property int narrowCoverSize: 96

    readonly property bool hasTrack: {
        const hasMetadata = root._trimmed(root.uiBridge.currentTrackTitle).length > 0
            || root._trimmed(root.uiBridge.currentTrackArtist).length > 0
            || root._trimmed(root.uiBridge.currentTrackAlbum).length > 0
        const hasActivePath = (root.uiBridge.playbackState || "") !== "Stopped"
            && root._trimmed(root.uiBridge.currentTrackPath).length > 0
        return hasMetadata || hasActivePath
    }
    readonly property string layoutMode: !root.hasTrack ? "empty" : (root.narrow ? "narrow" : "wide")

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
    readonly property string narrowDetailsText: root._joined([root.genreText, root.trackNumberText],
                                                             root.lineSeparators.narrowDetails)

    // The lines the panel shows, wide and narrow.
    readonly property var trackLines: ({
        title: root.titleText,
        artistAlbum: root.artistAlbumText,
        details: root.detailsText,
        artist: root.artistText,
        narrowAlbum: root.narrowAlbumText,
        narrowDetails: root.narrowDetailsText
    })

    // A track change cross-fades (#373 Motion), line by line: a line that
    // differs for the next track fades the one before out over it, in
    // place, and the fields at its start that the two share stay still; a
    // line that is the same stays still. The next track of the same album
    // fades over shortDuration; a track of another album, or with another
    // cover, fades the cover and the lines together on one curve over
    // longDuration. A line that comes to differ while the track change
    // fades (the metadata comes after the path) joins the fade from full
    // opacity, and the lines already fading go on from theirs. A cover
    // that changes later for the same track fades too: during a track
    // change it joins it, and every layer goes on from the opacity it has
    // onto the long curve; after one, the cover fades alone and the lines
    // change in place. What shows is always the playing track's; what the
    // panel showed before only fades out over it. Nothing fades into or
    // out of "Nothing playing": the first track after it settles its cover
    // and lines together, whatever order the bridge tells them in. With
    // animations off every change is instant.
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
    // in `trackLines` (see `_fadingLine`); the cover before, whether it
    // fades (`coverFade`) and its opacity then (`coverFrom`); and whether
    // the fade is a track change (`track`), which lines that come to
    // differ join.
    property var outgoing: null
    // How far the fade has come: 0 when it starts, 1 once the playing
    // track alone shows.
    property real handover: 1
    readonly property NumberAnimation handoverAnimation: NumberAnimation {
        objectName: "nowPlayingHandoverAnimation"
        target: root
        property: "handover"
        from: 0
        to: 1
        easing.type: Easing.OutCubic
        onFinished: root.outgoing = null
    }
    // The cover before goes from where it started to 0, on the one curve;
    // the playing track's cover shows whole under it.
    readonly property real outgoingCoverOpacity: root.outgoing !== null && root.outgoing.coverFade
        ? root.outgoing.coverFrom * (1 - root.handover) : 0

    // A line that fades: the text it showed before (`text`); the start of
    // it that stays still (`still`, see `_sharedStart`), set when the line
    // starts to fade and only ever cut shorter; the rest of the text before
    // in parts that fade out from the opacity each had when this fade
    // started (`parts`, [{start, from}], in order: each from the end of its
    // `start`, a start of `text`, to the next one's; the first at `still`);
    // and the playing track's text past `still`, which fades in as one,
    // from 1 - `incomingFrom`. The parts are cut where the text before has
    // gaps, which the playing track's text need not have there, so it is
    // never cut but at `still`, the end of the start both share.
    function _fadingLine(text, still, parts, incomingFrom) {
        return { text: text, still: still, parts: parts, incomingFrom: incomingFrom }
    }

    // The line before of `key` as it fades out, or null.
    function _outgoingLine(key) {
        return root.outgoing !== null ? (root.outgoing.lines[key] || null) : null
    }

    // The opacity of the part of the line before of `key` that shows most,
    // on the fade's curve: the playing track's makes up the rest of each
    // part.
    function _outgoingLineOpacity(key) {
        const line = root._outgoingLine(key)
        if (line === null)
            return 0
        return Math.max(...line.parts.map(part => part.from)) * (1 - root.handover)
    }

    // Whether `text` has a right-to-left character: its fields are then
    // not laid out from the left, so none of it stays still.
    function _hasRightToLeft(text) {
        return /[\u0590-\u08FF\uFB1D-\uFDFF\uFE70-\uFEFF\u200F\u202B\u202E\u2067]|[\uD802\uD803\uD83A\uD83B][\uDC00-\uDFFF]/
            .test(text)
    }

    // The start of two lines that can stay still while one fades into the
    // other: their whole leading fields that are the same, with the
    // separator after them when both go on. Empty when the first fields
    // differ, or when either has right-to-left text.
    function _sharedStart(before, after, separator) {
        if (root._hasRightToLeft(before) || root._hasRightToLeft(after))
            return ""
        if (separator.length === 0)
            return before === after ? before : ""
        const left = before.split(separator)
        const right = after.split(separator)
        let shared = 0
        while (shared < left.length && shared < right.length && left[shared] === right[shared])
            ++shared
        if (shared === 0)
            return ""
        const start = left.slice(0, shared).join(separator)
        return shared < left.length && shared < right.length ? start + separator.replace(/\s+$/, "") : start
    }

    // What of `line` still shows once the playing track's line is `next`:
    // the parts, and the playing track's text, go on from the opacity they
    // have; a field of the still start that `next` no longer shares fades
    // out from full opacity, as it showed, and the playing track's text
    // there fades in with the rest of it.
    function _goOn(line, next, separator) {
        const level = 1 - root.handover
        const parts = line.parts.map(part => ({ start: part.start, from: part.from * level }))
        const shared = root._sharedStart(line.text, next, separator)
        const still = shared.length < line.still.length ? shared : line.still
        if (still !== line.still)
            parts.unshift({ start: still, from: 1 })
        // Parts that fade alike are one.
        const merged = parts.filter((part, index) => index === 0 || part.from !== parts[index - 1].from)
        return root._fadingLine(line.text, still, merged, line.incomingFrom * level)
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
            // Into or out of "Nothing playing", whose layout differs: at once.
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
            root.startHandover(was, next)
            return
        }
        const coverChange = was.coverPath !== next.coverPath
        if (root.settling) {
            // The first track's metadata, in place.
            if (coverChange) {
                root.settling = false
            }
            return
        }
        const fading = root.outgoing
        if (coverChange || (fading !== null && !fading.coverFade && fading.album !== next.album)
                || (fading !== null && fading.track && root._stillChanges(was, next))) {
            // A cover that changed, an album named after the track itself
            // while only its lines fade, or a line, or a field of one, of
            // the track change that was still and now differs.
            root.joinHandover(was, next)
        }
    }

    // Whether a line that does not fade changed from `was` to `next`, or
    // one that fades no longer shares all of its still start.
    function _stillChanges(was, next) {
        for (const key in next.lines) {
            const line = root._outgoingLine(key)
            if (line !== null && root._outgoingLineOpacity(key) > 0) {
                const separator = root.lineSeparators[key] || ""
                if (root._sharedStart(line.text, next.lines[key], separator).length < line.still.length)
                    return true
            } else if (was.lines[key] !== next.lines[key]) {
                return true
            }
        }
        return false
    }

    // Another track: each line fades from what it shows, during a fade
    // whichever of the two shows more, when that differs from `next`'s;
    // the cover fades to `next`'s on the long curve when the cover or the
    // album differ. The lines that still show the track before's metadata
    // join once theirs comes.
    function startHandover(was, next) {
        const fading = root.outgoing
        const lines = {}
        for (const key in next.lines) {
            const before = root._outgoingLineOpacity(key) > 0.5
                ? root._outgoingLine(key).text : was.lines[key]
            if (before !== next.lines[key])
                lines[key] = root._newFadingLine(key, before, next.lines[key])
        }
        const coverBefore = fading !== null && root.outgoingCoverOpacity > 0.5
        const coverPath = coverBefore ? fading.coverPath : was.coverPath
        const album = coverBefore ? fading.album : was.album
        root.fade({
            lines: lines,
            coverPath: coverPath,
            album: album,
            coverFade: coverPath !== next.coverPath || album !== next.album,
            coverFrom: 1,
            track: true
        })
    }

    // A line that starts to fade from `before` to `after`, whole past their
    // shared start.
    function _newFadingLine(key, before, after) {
        const still = root._sharedStart(before, after, root.lineSeparators[key] || "")
        return root._fadingLine(before, still, [{ start: still, from: 1 }], 1)
    }

    // The same track's cover, album or, during a track change, a line
    // changed: the fade starts again, the cover on the long curve when it
    // differs, from the one that shows; the lines still fading go on from
    // the opacity each part has, and what of a line of the track change
    // showed whole and now differs fades from full opacity. No layer jumps.
    function joinHandover(was, next) {
        const fading = root.outgoing
        const track = fading !== null && fading.track
        const lines = {}
        for (const key in next.lines) {
            if (root._outgoingLineOpacity(key) > 0) {
                lines[key] = root._goOn(root._outgoingLine(key), next.lines[key],
                                        root.lineSeparators[key] || "")
            } else if (track && was.lines[key] !== next.lines[key]) {
                lines[key] = root._newFadingLine(key, was.lines[key], next.lines[key])
            }
        }
        const coverFading = fading !== null && fading.coverFade && root.outgoingCoverOpacity > 0
        const coverPath = coverFading ? fading.coverPath : was.coverPath
        const album = coverFading ? fading.album : was.album
        root.fade({
            lines: lines,
            coverPath: coverPath,
            album: album,
            coverFade: coverPath !== next.coverPath || album !== next.album,
            coverFrom: coverFading ? root.outgoingCoverOpacity : 1,
            track: track
        })
    }

    // Runs the fade of `out`. A track change runs even before any of its
    // lines differs, for the lines its metadata changes once it comes.
    function fade(out) {
        const duration = out.coverFade ? Kirigami.Units.longDuration : Kirigami.Units.shortDuration
        const fades = out.track || out.coverFade || Object.keys(out.lines).length > 0
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
    Component.onCompleted: root.followTrack()

    // 15, 14 and 12 px with the default 13 px font.
    readonly property real titlePointSize: root.uiPalette.basePointSize * 15 / 13
    readonly property real narrowTitlePointSize: root.uiPalette.basePointSize * 14 / 13

    implicitHeight: root.layoutMode === "empty"
        ? emptyRow.height + 2 * Kirigami.Units.smallSpacing + root.textInset
        : (root.layoutMode === "narrow"
            ? root.narrowCoverSize + root.textInset
            : root.width + liveText.wideHeight + root.textInset + 14)

    Accessible.role: Accessible.Grouping
    Accessible.name: I18n.i18nc("@label", "Now playing")

    function _trimmed(value) {
        return String(value || "").trim()
    }

    function _joined(parts, separator) {
        return parts.filter(part => part.length > 0).join(separator)
    }

    // The text of a line, as each of its layers draws it: elided, on the
    // whole width of the line, so the layers lay it out alike.
    component LineText: Text {
        required property var owner
        width: owner.width
        elide: Text.ElideRight
        maximumLineCount: 1
        textFormat: Text.PlainText
        font.family: root.uiPalette.baseFont.family
        font.pointSize: owner.pointSize
        font.weight: owner.weight
        font.features: owner.features
        color: owner.color
        lineHeightMode: owner.fixedLineHeight > 0 ? Text.FixedHeight : Text.ProportionalHeight
        lineHeight: owner.fixedLineHeight > 0 ? owner.fixedLineHeight : 1
    }

    // A line of the playing track, with its text as the accessible name.
    // While a track change fades it, the line before fades out over it
    // past the fields at the start that the two share; those stay still,
    // drawn by the playing track's own text, clipped, so nothing in them
    // moves or dims. Past them the playing track's text fades in as one,
    // and each part of the line before fades out on its own (see
    // `_fadingLine`), between edges in its own gaps.
    component FadingLine: Item {
        id: line

        // The line's key in `trackLines`.
        required property string lineKey
        required property string text
        // Its items' names: "nowPlaying<name>" for the playing track's
        // text, "nowPlayingIncoming<name>" for the part of it that fades in,
        // and "nowPlayingOutgoing<name>Part" for each part of the line
        // before, whose text is "nowPlayingOutgoing<name>".
        required property string name
        property real pointSize: root.uiPalette.basePointSize
        property int weight: Font.Normal
        property var features: ({})
        property color color: root.uiPalette.textSecondary
        // A line height in pixels, or 0 for the font's.
        property real fixedLineHeight: 0
        readonly property bool truncated: stillText.truncated

        readonly property var before: root.outgoing !== null
            ? (root.outgoing.lines[line.lineKey] || null) : null
        readonly property bool fading: line.before !== null && root.handover < 1
        readonly property string outgoingText: line.fading ? line.before.text : ""
        // The playing track's text past the still start, fading in.
        readonly property real incomingOpacity: line.fading
            ? 1 - line.before.incomingFrom * (1 - root.handover) : 1
        // How much of the line stays still, from the left: up to the middle
        // of the space after the still start, where neither has a glyph; all
        // of it when nothing fades.
        readonly property real stillWidth: {
            if (!line.fading)
                return line.width
            const still = line.before.still
            if (still.length === 0)
                return 0
            if (line._keeps(line.before.text, still) && line._keeps(line.text, still))
                return line._edgeAfter(still)
            // Elided within the still start: the two look the same when
            // both are, and differ there otherwise.
            const elidedWithin = metrics.advanceWidth(still) + metrics.advanceWidth("\u2026") > line.width
            return elidedWithin && line._cutOff(line.before.text) && line._cutOff(line.text) ? line.width : 0
        }
        // Where each part of the line before starts, then the end of the
        // line. A part that would start where the line before is elided
        // starts at the end, so the ellipsis is in one part.
        readonly property var partEdges: {
            if (!line.fading)
                return []
            const text = line.before.text
            const edges = line.before.parts.map((part, index) => index === 0
                ? line.stillWidth
                : (line._keeps(text, part.start)
                    ? Math.min(line.width, Math.max(line.stillWidth, line._edgeAfter(part.start)))
                    : line.width))
            edges.push(line.width)
            return edges
        }

        // The middle of the space after `start`, a start of the line
        // before, as the line before lays it out.
        function _edgeAfter(start) {
            // Read the font, for the edges to follow it.
            metrics.font
            return (metrics.advanceWidth(start) + metrics.advanceWidth(start + " ")) / 2
        }

        function _cutOff(text) {
            return metrics.advanceWidth(text) > line.width
        }

        // Whether `text`, elided to the line, still shows `start`, the
        // space after it and the glyph after that, so that the gap after
        // `start` is one with no ellipsis in it.
        function _keeps(text, start) {
            return !line._cutOff(text)
                || metrics.advanceWidth(text.slice(0, start.length + 2)) + metrics.advanceWidth("\u2026")
                    <= line.width
        }

        Layout.fillWidth: true
        implicitHeight: stillText.implicitHeight
        visible: line.text.length > 0 || line.outgoingText.length > 0

        FontMetrics {
            id: metrics
            font: stillText.font
        }

        // What stays still: the playing track's text up to stillWidth.
        Item {
            width: line.stillWidth
            height: line.height
            clip: line.fading

            LineText {
                id: stillText
                objectName: "nowPlaying" + line.name
                owner: line
                text: line.text
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }

        // What changes: the playing track's text past stillWidth, fading in
        // as one, and over it the line before's, fading out part by part.
        Item {
            objectName: "nowPlayingIncoming" + line.name
            x: line.stillWidth
            width: Math.max(0, line.width - line.stillWidth)
            height: line.height
            clip: true
            visible: line.fading
            opacity: line.incomingOpacity

            LineText {
                x: -line.stillWidth
                owner: line
                text: line.text
                Accessible.ignored: true
            }
        }
        Repeater {
            model: line.fading ? line.before.parts : []

            delegate: Item {
                id: part

                required property var modelData
                required property int index
                readonly property real start: line.partEdges[part.index] || 0

                objectName: "nowPlayingOutgoing" + line.name + "Part"
                x: part.start
                width: Math.max(0, (line.partEdges[part.index + 1] || 0) - part.start)
                height: line.height
                clip: true
                opacity: part.modelData.from * (1 - root.handover)

                LineText {
                    objectName: "nowPlayingOutgoing" + line.name
                    x: -part.start
                    owner: line
                    text: line.outgoingText
                    Accessible.ignored: true
                }
            }
        }
    }

    // Nothing loaded.
    Item {
        id: emptyRow
        objectName: "nowPlayingEmptyRow"
        visible: root.layoutMode === "empty"
        x: 0
        y: Kirigami.Units.smallSpacing
        width: root.width
        height: root.uiPalette.rowHeight

        Kirigami.Icon {
            x: root.textInset
            anchors.verticalCenter: parent.verticalCenter
            width: Kirigami.Units.iconSizes.small
            height: width
            source: "media-album-track"
            isMask: true
            color: root.uiPalette.textTertiary
        }

        Text {
            objectName: "nowPlayingEmptyLabel"
            x: root.textInset + Kirigami.Units.iconSizes.small + Kirigami.Units.largeSpacing
            width: Math.max(0, parent.width - x - root.textInset)
            anchors.verticalCenter: parent.verticalCenter
            text: I18n.i18nc("@info", "Nothing playing")
            elide: Text.ElideRight
            font: root.uiPalette.baseFont
            color: root.uiPalette.textSecondary
        }
    }

    Components.AlbumArtTile {
        id: cover
        objectName: "nowPlayingCover"
        visible: root.layoutMode !== "empty"
        x: 0
        y: 0
        width: root.layoutMode === "narrow" ? root.narrowCoverSize : root.width
        height: width
        uiBridge: root.uiBridge
        placeholderColor: Qt.rgba(root.uiPalette.text.r, root.uiPalette.text.g,
                                  root.uiPalette.text.b, 0.04)
        placeholderGlyphColor: root.uiPalette.textTertiary
        replaceFromItunesAction: root.replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: root.currentTrackItunesArtworkDisabledReason
        openAlbumArtViewer: root.openAlbumArtViewer
        // The cover before, over this one while it fades out.
        outgoingShown: root.outgoing !== null && root.outgoing.coverFade
        outgoingCoverPath: cover.outgoingShown ? root.outgoing.coverPath : ""
        outgoingOpacity: root.outgoingCoverOpacity
    }

    // The playing track's lines, each of which fades on its own over a
    // track change.
    Item {
        id: liveText

        readonly property real wideHeight: wideColumn.implicitHeight
        readonly property bool truncated: root.layoutMode === "wide"
            ? (wideTitle.truncated || wideArtistAlbum.truncated || wideDetails.truncated)
            : (root.layoutMode === "narrow"
                && (narrowTitle.truncated || narrowArtist.truncated
                    || narrowAlbum.truncated || narrowDetails.truncated))
        readonly property bool hovered: wideHover.hovered || narrowHover.hovered
        readonly property Item wideItem: wideColumn
        readonly property Item narrowItem: narrowColumn

        visible: root.layoutMode !== "empty"
        x: 0
        y: 0
        width: root.width
        height: root.height

        // Wide: the text block under the cover.
        ColumnLayout {
            id: wideColumn
            objectName: "nowPlayingWideText"
            visible: root.layoutMode === "wide"
            x: root.textInset
            y: cover.height + root.textInset
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
                pointSize: root.titlePointSize
                weight: Font.DemiBold
                color: root.uiPalette.text
                fixedLineHeight: Math.round(root.uiPalette.rowHeight * 20 / 26)
            }
            FadingLine {
                id: wideArtistAlbum
                lineKey: "artistAlbum"
                name: "ArtistAlbum"
                text: root.trackLines.artistAlbum
            }
            FadingLine {
                id: wideDetails
                lineKey: "details"
                name: "Details"
                text: root.trackLines.details
                pointSize: root.uiPalette.smallPointSize
                features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }
        }

        // Narrow: four short lines beside the cover.
        ColumnLayout {
            id: narrowColumn
            objectName: "nowPlayingNarrowText"
            visible: root.layoutMode === "narrow"
            x: root.narrowCoverSize + root.textInset
            y: 10
            width: Math.max(0, root.width - x - root.textInset)
            spacing: 0

            HoverHandler {
                id: narrowHover
            }

            FadingLine {
                id: narrowTitle
                lineKey: "title"
                name: "NarrowTitle"
                text: root.trackLines.title
                pointSize: root.narrowTitlePointSize
                weight: Font.DemiBold
                color: root.uiPalette.text
            }
            FadingLine {
                id: narrowArtist
                lineKey: "artist"
                name: "NarrowArtist"
                text: root.trackLines.artist
                pointSize: root.uiPalette.smallPointSize
                Layout.topMargin: 2
            }
            FadingLine {
                id: narrowAlbum
                lineKey: "narrowAlbum"
                name: "NarrowAlbum"
                text: root.trackLines.narrowAlbum
                pointSize: root.uiPalette.smallPointSize
                features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }
            FadingLine {
                id: narrowDetails
                lineKey: "narrowDetails"
                name: "NarrowDetails"
                text: root.trackLines.narrowDetails
                pointSize: root.uiPalette.smallPointSize
                features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }
        }
    }

    // The whole text of the lines, when one of them is cut off.
    readonly property bool textTruncated: liveText.truncated

    ToolTip {
        parent: root.layoutMode === "wide" ? liveText.wideItem : liveText.narrowItem
        visible: liveText.hovered && root.textTruncated
        delay: Kirigami.Units.toolTipDelay
        text: root.layoutMode === "wide"
            ? root._joined([root.titleText, root.artistAlbumText, root.detailsText], "\n")
            : root._joined([root.titleText, root.artistText, root.narrowAlbumText,
                            root.narrowDetailsText], "\n")
    }
}
