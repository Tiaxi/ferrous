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
    // What fades out, or null: `lines`, the text each line that fades
    // showed before and the opacity it had when this fade started, by its
    // key in `trackLines` ({text, from}); the cover before, whether it
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

    // The line before of `key` as it fades out, or null.
    function _outgoingLine(key) {
        return root.outgoing !== null ? (root.outgoing.lines[key] || null) : null
    }

    // The opacity of the line before of `key`, on the same curve: the
    // playing track's makes up the rest where the two differ.
    function _outgoingLineOpacity(key) {
        const line = root._outgoingLine(key)
        return line !== null ? line.from * (1 - root.handover) : 0
    }

    // The start of two lines that stays still while one fades into the
    // other: their whole leading fields that are the same, with the
    // separator after them when both go on. Empty when the first fields
    // differ.
    function _sharedStart(before, after, separator) {
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
                || (fading !== null && fading.track && root._lineComesToDiffer(was, next))) {
            // A cover that changed, an album named after the track itself
            // while only its lines fade, or a line of the track change that
            // was still and now differs.
            root.joinHandover(was, next)
        }
    }

    // Whether a line that does not fade changed from `was` to `next`.
    function _lineComesToDiffer(was, next) {
        for (const key in next.lines) {
            if (root._outgoingLineOpacity(key) <= 0 && was.lines[key] !== next.lines[key])
                return true
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
                lines[key] = { text: before, from: 1 }
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

    // The same track's cover, album or, during a track change, a line
    // changed: the fade starts again, the cover on the long curve when it
    // differs, from the one that shows; the lines still fading go on from
    // their opacity, and a line of the track change that comes to differ
    // fades from what it showed, whole. No layer jumps.
    function joinHandover(was, next) {
        const fading = root.outgoing
        const track = fading !== null && fading.track
        const lines = {}
        for (const key in next.lines) {
            const opacity = root._outgoingLineOpacity(key)
            if (opacity > 0) {
                lines[key] = { text: root._outgoingLine(key).text, from: opacity }
            } else if (track && was.lines[key] !== next.lines[key]) {
                lines[key] = { text: was.lines[key], from: 1 }
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
    // moves or dims.
    component FadingLine: Item {
        id: line

        // The line's key in `trackLines`.
        required property string lineKey
        required property string text
        // Its items' names: "nowPlaying<name>" for the playing track's
        // text, "nowPlayingOutgoing<name>" for the line before.
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
        // The line before's opacity, on the fade's curve; the playing
        // track's text makes up the rest past `stillWidth`.
        readonly property real outgoingOpacity: line.before !== null
            ? line.before.from * (1 - root.handover) : 0
        readonly property bool fading: line.outgoingOpacity > 0
        readonly property string outgoingText: line.fading ? line.before.text : ""
        readonly property string stillStart: line.fading
            ? root._sharedStart(line.outgoingText, line.text, root.lineSeparators[line.lineKey] || "")
            : ""
        // How much of the line stays still, from the left: up to the middle
        // of the space after the shared start, where no glyph is; all of it
        // when nothing fades.
        readonly property real stillWidth: {
            if (!line.fading)
                return line.width
            if (line.stillStart.length === 0)
                return 0
            const edge = (stillMetrics.advanceWidth + stillSpaceMetrics.advanceWidth) / 2
            if (edge + ellipsisMetrics.advanceWidth <= line.width)
                return edge
            // Elided within the shared start: the two look the same when
            // both are, and differ there otherwise.
            return stillText.truncated && outgoingLineText.truncated ? line.width : 0
        }

        Layout.fillWidth: true
        implicitHeight: stillText.implicitHeight
        visible: line.text.length > 0 || line.outgoingText.length > 0

        TextMetrics {
            id: stillMetrics
            font: stillText.font
            text: line.stillStart
        }
        TextMetrics {
            id: stillSpaceMetrics
            font: stillText.font
            text: line.stillStart.length > 0 ? line.stillStart + " " : ""
        }
        TextMetrics {
            id: ellipsisMetrics
            font: stillText.font
            text: line.stillStart.length > 0 ? "\u2026" : ""
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

        // What changes: the playing track's text past stillWidth, fading
        // in, and the line before's, fading out over it.
        Item {
            objectName: "nowPlayingIncoming" + line.name
            x: line.stillWidth
            width: Math.max(0, line.width - line.stillWidth)
            height: line.height
            clip: true
            visible: line.fading
            opacity: 1 - line.outgoingOpacity

            LineText {
                x: -line.stillWidth
                owner: line
                text: line.text
                Accessible.ignored: true
            }
        }
        Item {
            objectName: "nowPlayingOutgoing" + line.name + "Part"
            x: line.stillWidth
            width: Math.max(0, line.width - line.stillWidth)
            height: line.height
            clip: true
            visible: line.fading
            opacity: line.outgoingOpacity

            LineText {
                id: outgoingLineText
                objectName: "nowPlayingOutgoing" + line.name
                x: -line.stillWidth
                owner: line
                text: line.outgoingText
                Accessible.ignored: true
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
