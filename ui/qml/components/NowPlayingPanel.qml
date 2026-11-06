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
    readonly property string trackNumberText: root.uiBridge.currentTrackNumber > 0
        ? I18n.i18nc("@info the playing track's number on its album", "Track %1",
                     root.uiBridge.currentTrackNumber)
        : ""
    // Missing values are left out rather than shown as dashes.
    readonly property string artistAlbumText: root._joined([root.artistText, root.albumText], " — ")
    readonly property string detailsText: root._joined(
        [root.yearText, root.genreText, root.trackNumberText], " · ")
    readonly property string narrowAlbumText: root.albumText.length > 0 && root.yearText.length > 0
        ? I18n.i18nc("@info album and its year", "%1 (%2)", root.albumText, root.yearText)
        : (root.albumText.length > 0 ? root.albumText : root.yearText)
    readonly property string narrowDetailsText: root._joined([root.genreText, root.trackNumberText], " · ")

    // The lines the panel shows, wide and narrow.
    readonly property var trackLines: ({
        title: root.titleText,
        artistAlbum: root.artistAlbumText,
        details: root.detailsText,
        artist: root.artistText,
        narrowAlbum: root.narrowAlbumText,
        narrowDetails: root.narrowDetailsText
    })

    readonly property var noLines: ({
        title: "", artistAlbum: "", details: "", artist: "", narrowAlbum: "", narrowDetails: ""
    })

    // A track change cross-fades (#373 Motion): the next track of the same
    // album fades its lines in over the old ones over shortDuration; a
    // track of another album, or with another cover, fades the cover and
    // the lines together on one curve over longDuration. A cover that
    // changes later for the same track fades too: during a fade of the
    // lines it joins them, and every layer goes on from the opacity it has
    // onto the long curve; after one, the cover fades alone. What shows is
    // always the playing track's; what the panel showed before only fades
    // out over it. Other metadata arriving or changing later changes the
    // lines in place. Nothing fades into or out of "Nothing playing": the
    // first track after it settles its cover and lines together, whatever
    // order the bridge tells them in. With animations off every change is
    // instant.
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
    // What fades out: the lines and the cover the panel showed before,
    // which of the two fade (`linesFade`, `coverFade`), and the opacity
    // each had when this fade started (`linesFrom`, `coverFrom`); or null.
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
    // The layers' opacities: the lines and the cover before go from where
    // they started to 0, on the one curve; the playing track's lines make
    // up the rest. The playing track's cover shows whole under the one
    // before.
    readonly property real outgoingLinesOpacity: root.outgoing !== null && root.outgoing.linesFade
        ? root.outgoing.linesFrom * (1 - root.handover) : 0
    readonly property real outgoingCoverOpacity: root.outgoing !== null && root.outgoing.coverFade
        ? root.outgoing.coverFrom * (1 - root.handover) : 0

    function _sameLines(a, b) {
        return a.title === b.title && a.artistAlbum === b.artistAlbum && a.details === b.details
            && a.artist === b.artist && a.narrowAlbum === b.narrowAlbum
            && a.narrowDetails === b.narrowDetails
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
        if (coverChange || (fading !== null && !fading.coverFade && fading.album !== next.album)) {
            // A cover that changed, or an album named after the track
            // itself while only its lines fade.
            root.joinHandover(was, next)
        }
    }

    // Another track: fades from what the panel shows, during a fade
    // whichever of the two shows more, to `next`'s lines, and to its cover
    // on the long curve when the cover or the album differ.
    function startHandover(was, next) {
        const fading = root.outgoing
        const linesBefore = fading !== null && root.outgoingLinesOpacity > 0.5
        const coverBefore = fading !== null && root.outgoingCoverOpacity > 0.5
        const coverPath = coverBefore ? fading.coverPath : was.coverPath
        const album = coverBefore ? fading.album : was.album
        root.fade({
            lines: linesBefore ? fading.lines : was.lines,
            coverPath: coverPath,
            album: album,
            linesFade: true,
            coverFade: coverPath !== next.coverPath || album !== next.album,
            linesFrom: 1,
            coverFrom: 1
        })
    }

    // The same track's cover or album changed: the cover fades on the long
    // curve, from the one that shows, and lines still fading go on from
    // their opacity onto it, so no layer jumps.
    function joinHandover(was, next) {
        const fading = root.outgoing
        const linesFrom = root.outgoingLinesOpacity
        const coverFading = fading !== null && fading.coverFade && root.outgoingCoverOpacity > 0
        const coverPath = coverFading ? fading.coverPath : was.coverPath
        const album = coverFading ? fading.album : was.album
        root.fade({
            lines: linesFrom > 0 ? fading.lines : next.lines,
            coverPath: coverPath,
            album: album,
            linesFade: linesFrom > 0,
            coverFade: coverPath !== next.coverPath || album !== next.album,
            linesFrom: linesFrom,
            coverFrom: coverFading ? root.outgoingCoverOpacity : 1
        })
    }

    function fade(out) {
        const duration = out.coverFade ? Kirigami.Units.longDuration : Kirigami.Units.shortDuration
        if ((!out.linesFade && !out.coverFade) || duration <= 0) {
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

    // A line of the details: elided, with its text as the accessible name.
    component DetailLine: Text {
        Layout.fillWidth: true
        visible: text.length > 0
        elide: Text.ElideRight
        maximumLineCount: 1
        textFormat: Text.PlainText
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
        color: root.uiPalette.textSecondary
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // The lines of one track, wide or narrow: the playing track's, and the
    // track before's while it fades out over them.
    component TrackText: Item {
        id: block

        // The lines, as `trackLines` has them.
        required property var lines
        // The start of its items' names: "nowPlaying" for the playing
        // track's.
        required property string namePrefix
        readonly property real wideHeight: wideColumn.implicitHeight
        readonly property bool truncated: root.layoutMode === "wide"
            ? (wideTitle.truncated || wideArtistAlbum.truncated || wideDetails.truncated)
            : (root.layoutMode === "narrow"
                && (narrowTitle.truncated || narrowArtist.truncated
                    || narrowAlbum.truncated || narrowDetails.truncated))
        readonly property bool hovered: wideHover.hovered || narrowHover.hovered
        readonly property Item wideItem: wideColumn
        readonly property Item narrowItem: narrowColumn

        x: 0
        y: 0
        width: root.width
        height: root.height

        // Wide: the text block under the cover.
        ColumnLayout {
            id: wideColumn
            objectName: block.namePrefix + "WideText"
            visible: root.layoutMode === "wide"
            x: root.textInset
            y: cover.height + root.textInset
            width: Math.max(0, root.width - 2 * root.textInset)
            spacing: 2

            HoverHandler {
                id: wideHover
            }

            DetailLine {
                id: wideTitle
                objectName: block.namePrefix + "Title"
                text: block.lines.title
                font.pointSize: root.titlePointSize
                font.weight: Font.DemiBold
                color: root.uiPalette.text
                lineHeightMode: Text.FixedHeight
                lineHeight: Math.round(root.uiPalette.rowHeight * 20 / 26)
            }
            DetailLine {
                id: wideArtistAlbum
                objectName: block.namePrefix + "ArtistAlbum"
                text: block.lines.artistAlbum
            }
            DetailLine {
                id: wideDetails
                objectName: block.namePrefix + "Details"
                text: block.lines.details
                font.pointSize: root.uiPalette.smallPointSize
                font.features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }
        }

        // Narrow: four short lines beside the cover.
        ColumnLayout {
            id: narrowColumn
            objectName: block.namePrefix + "NarrowText"
            visible: root.layoutMode === "narrow"
            x: root.narrowCoverSize + root.textInset
            y: 10
            width: Math.max(0, root.width - x - root.textInset)
            spacing: 0

            HoverHandler {
                id: narrowHover
            }

            DetailLine {
                id: narrowTitle
                objectName: block.namePrefix + "NarrowTitle"
                text: block.lines.title
                font.pointSize: root.narrowTitlePointSize
                font.weight: Font.DemiBold
                color: root.uiPalette.text
            }
            DetailLine {
                id: narrowArtist
                text: block.lines.artist
                font.pointSize: root.uiPalette.smallPointSize
                Layout.topMargin: 2
            }
            DetailLine {
                id: narrowAlbum
                text: block.lines.narrowAlbum
                font.pointSize: root.uiPalette.smallPointSize
                font.features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }
            DetailLine {
                id: narrowDetails
                text: block.lines.narrowDetails
                font.pointSize: root.uiPalette.smallPointSize
                font.features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
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

    // The playing track's lines, which fade in over a track change.
    TrackText {
        id: liveText
        visible: root.layoutMode !== "empty"
        lines: root.trackLines
        namePrefix: "nowPlaying"
        opacity: 1 - root.outgoingLinesOpacity
    }

    // The lines before, fading out.
    TrackText {
        id: outgoingText
        readonly property bool fades: root.outgoing !== null && root.outgoing.linesFade
        visible: outgoingText.fades && root.layoutMode !== "empty"
        lines: outgoingText.fades ? root.outgoing.lines : root.noLines
        namePrefix: "nowPlayingOutgoing"
        opacity: root.outgoingLinesOpacity
        Accessible.ignored: true
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
