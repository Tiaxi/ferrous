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

    // 15, 14 and 12 px with the default 13 px font.
    readonly property real titlePointSize: root.uiPalette.basePointSize * 15 / 13
    readonly property real narrowTitlePointSize: root.uiPalette.basePointSize * 14 / 13

    implicitHeight: root.layoutMode === "empty"
        ? emptyRow.height + 2 * Kirigami.Units.smallSpacing + root.textInset
        : (root.layoutMode === "narrow"
            ? root.narrowCoverSize + root.textInset
            : root.width + wideText.implicitHeight + root.textInset + 14)

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
    }

    // Wide: the text block under the cover.
    ColumnLayout {
        id: wideText
        objectName: "nowPlayingWideText"
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
            objectName: "nowPlayingTitle"
            text: root.titleText
            font.pointSize: root.titlePointSize
            font.weight: Font.DemiBold
            color: root.uiPalette.text
            lineHeightMode: Text.FixedHeight
            lineHeight: Math.round(root.uiPalette.rowHeight * 20 / 26)
        }
        DetailLine {
            id: wideArtistAlbum
            objectName: "nowPlayingArtistAlbum"
            text: root.artistAlbumText
        }
        DetailLine {
            id: wideDetails
            objectName: "nowPlayingDetails"
            text: root.detailsText
            font.pointSize: root.uiPalette.smallPointSize
            font.features: root.uiPalette.tabularFigures
            color: root.uiPalette.textTertiary
        }
    }

    // Narrow: four short lines beside the cover.
    ColumnLayout {
        id: narrowText
        objectName: "nowPlayingNarrowText"
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
            objectName: "nowPlayingNarrowTitle"
            text: root.titleText
            font.pointSize: root.narrowTitlePointSize
            font.weight: Font.DemiBold
            color: root.uiPalette.text
        }
        DetailLine {
            id: narrowArtist
            text: root.artistText
            font.pointSize: root.uiPalette.smallPointSize
            Layout.topMargin: 2
        }
        DetailLine {
            id: narrowAlbum
            text: root.narrowAlbumText
            font.pointSize: root.uiPalette.smallPointSize
            font.features: root.uiPalette.tabularFigures
            color: root.uiPalette.textTertiary
        }
        DetailLine {
            id: narrowDetails
            text: root.narrowDetailsText
            font.pointSize: root.uiPalette.smallPointSize
            font.features: root.uiPalette.tabularFigures
            color: root.uiPalette.textTertiary
        }
    }

    // The whole text of the lines, when one of them is cut off.
    readonly property bool textTruncated: root.layoutMode === "wide"
        ? (wideTitle.truncated || wideArtistAlbum.truncated || wideDetails.truncated)
        : (root.layoutMode === "narrow"
            && (narrowTitle.truncated || narrowArtist.truncated
                || narrowAlbum.truncated || narrowDetails.truncated))

    ToolTip {
        parent: root.layoutMode === "wide" ? wideText : narrowText
        visible: (wideHover.hovered || narrowHover.hovered) && root.textTruncated
        delay: Kirigami.Units.toolTipDelay
        text: root.layoutMode === "wide"
            ? root._joined([root.titleText, root.artistAlbumText, root.detailsText], "\n")
            : root._joined([root.titleText, root.artistText, root.narrowAlbumText,
                            root.narrowDetailsText], "\n")
    }
}
