import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "." as Components
import "../logic/FormatUtils.js" as FormatUtils

Components.SurfaceCard {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var queueTrackNumberText

    readonly property bool hasTrackContext: {
        const hasResolvedMetadata = (root.uiBridge.currentTrackTitle || "").trim().length > 0
            || (root.uiBridge.currentTrackArtist || "").trim().length > 0
            || (root.uiBridge.currentTrackAlbum || "").trim().length > 0
        const playbackState = (root.uiBridge.playbackState || "").trim()
        const hasActivePath = playbackState !== "Stopped"
            && (root.uiBridge.currentTrackPath || "").trim().length > 0
        return hasResolvedMetadata || hasActivePath
    }
    readonly property string marqueeResetKey: {
        return (root.uiBridge.currentTrackPath || "")
            + "|"
            + (root.uiBridge.currentTrackTitle || "")
            + "|"
            + (root.uiBridge.currentTrackArtist || "")
            + "|"
            + (root.uiBridge.currentTrackAlbum || "")
    }
    readonly property string resolvedTitle: {
        if (!hasTrackContext) {
            return "No track loaded"
        }
        const explicitTitle = (root.uiBridge.currentTrackTitle || "").trim()
        if (explicitTitle.length > 0) {
            return explicitTitle
        }
        const pathValue = (root.uiBridge.currentTrackPath || "").trim()
        if (pathValue.length > 0) {
            return FormatUtils.basenameFromPath(pathValue)
        }
        return "Nothing playing"
    }
    readonly property string resolvedArtist: {
        if (!hasTrackContext) {
            return "—"
        }
        const artistValue = (root.uiBridge.currentTrackArtist || "").trim()
        return artistValue.length > 0 ? artistValue : "Unknown artist"
    }
    readonly property string resolvedAlbum: {
        if (!hasTrackContext) {
            return "—"
        }
        const albumValue = (root.uiBridge.currentTrackAlbum || "").trim()
        return albumValue.length > 0 ? albumValue : "Unknown album"
    }
    readonly property string resolvedGenre: {
        if (!hasTrackContext) {
            return "—"
        }
        const genreValue = (root.uiBridge.currentTrackGenre || "").trim()
        return genreValue.length > 0 ? genreValue : "Unknown genre"
    }
    readonly property string resolvedTrackNumber: {
        if (!hasTrackContext) {
            return "—"
        }
        if (root.uiBridge.playingQueueIndex !== undefined
                && root.uiBridge.playingQueueIndex !== null
                && root.uiBridge.playingQueueIndex >= 0) {
            return root.queueTrackNumberText(root.uiBridge.playingQueueIndex)
        }
        if (root.uiBridge.selectedQueueIndex !== undefined
                && root.uiBridge.selectedQueueIndex !== null
                && root.uiBridge.selectedQueueIndex >= 0) {
            return root.queueTrackNumberText(root.uiBridge.selectedQueueIndex)
        }
        return "--"
    }
    readonly property string resolvedYear: {
        if (!hasTrackContext) {
            return "—"
        }
        const yearValue = root.uiBridge.currentTrackYear
        if (yearValue !== undefined && yearValue !== null && String(yearValue).length > 0) {
            return String(yearValue)
        }
        return "----"
    }

    color: root.uiPalette.uiSurfaceRaisedColor
    borderColor: root.uiPalette.uiBorderColor
    implicitHeight: nowPlayingColumn.implicitHeight + 12

    ColumnLayout {
        id: nowPlayingColumn
        anchors.fill: parent
        anchors.margins: 6
        spacing: 2

        Components.MetadataMarqueeRow {
            labelText: "Title:"
            valueText: root.resolvedTitle
            resetKey: root.marqueeResetKey
            labelColor: root.uiPalette.uiMutedTextColor
            textColor: root.uiPalette.uiTextColor
            emphasized: true
        }

        Components.MetadataMarqueeRow {
            labelText: "Artist:"
            valueText: root.resolvedArtist
            resetKey: root.marqueeResetKey
            labelColor: root.uiPalette.uiMutedTextColor
            textColor: root.uiPalette.uiTextColor
        }

        Components.MetadataMarqueeRow {
            labelText: "Album:"
            valueText: root.resolvedAlbum
            resetKey: root.marqueeResetKey
            labelColor: root.uiPalette.uiMutedTextColor
            textColor: root.uiPalette.uiTextColor
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "Track:"
                Layout.preferredWidth: 44
                horizontalAlignment: Text.AlignRight
                color: root.uiPalette.uiMutedTextColor
                font.pixelSize: 12
            }

            Label {
                Layout.fillWidth: true
                text: root.resolvedTrackNumber
                elide: Text.ElideRight
                color: root.uiPalette.uiTextColor
                font.pixelSize: 12
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "Year:"
                Layout.preferredWidth: 44
                horizontalAlignment: Text.AlignRight
                color: root.uiPalette.uiMutedTextColor
                font.pixelSize: 12
            }

            Label {
                Layout.fillWidth: true
                text: root.resolvedYear
                elide: Text.ElideRight
                color: root.uiPalette.uiTextColor
                font.pixelSize: 12
            }
        }

        Components.MetadataMarqueeRow {
            labelText: "Genre:"
            valueText: root.resolvedGenre
            resetKey: root.marqueeResetKey
            labelColor: root.uiPalette.uiMutedTextColor
            textColor: root.uiPalette.uiTextColor
        }
    }
}
