// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

// The playing track's cover, edge to edge: no inset, radius or frame. A
// track without a cover keeps the tile's place, on the colour around it,
// with a small note mark at its centre: the cover is missing, and nothing
// more is said about it (#373 H1).
Rectangle {
    id: root

    required property var uiBridge
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer
    // The tile and its mark while the track has no cover: the now-playing
    // panel passes the palette's.
    property color placeholderColor: "transparent"
    property color placeholderGlyphColor: Kirigami.Theme.textColor
    // The mark's size: 48 px, and a third of a smaller tile.
    readonly property real placeholderGlyphSize: Math.min(Kirigami.Units.iconSizes.large,
        Math.round(Math.min(root.width, root.height) / 3))
    readonly property url placeholderGlyphSource: Qt.resolvedUrl("../assets/cover-missing.svg")
    // What shows around a cover that does not fill the tile, or through
    // one that is partly transparent: the colour behind the tile, which the
    // now-playing panel passes. Each cover draws on it, so a cover at full
    // opacity hides all under it, the covers fading out under it too.
    property color backdropColor: "transparent"
    readonly property bool hasCover: root.uiBridge.currentTrackCoverPath.length > 0
    // The covers before, or their placeholder for none, over this one
    // while they fade out: the now-playing panel's album change. Oldest
    // first, each {coverPath, from}, showing at `from` times
    // `outgoingLevel`, and over the ones after it.
    property var outgoingCovers: []
    property real outgoingLevel: 0
    // The most covers before that fade out at once.
    property int outgoingSlots: 0
    readonly property real coverSourceSize: Math.max(256, root.width)

    implicitWidth: 320
    implicitHeight: 320
    Layout.fillWidth: true
    Layout.preferredHeight: width

    color: root.hasCover ? root.backdropColor : root.placeholderColor
    Accessible.role: Accessible.Graphic
    Accessible.name: root.hasCover
        ? I18n.i18nc("@info", "Album art")
        : I18n.i18nc("@info", "No album art")

    Image {
        anchors.fill: parent
        source: root.uiBridge.currentTrackCoverPath
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        cache: true
        retainWhileLoading: true
        sourceSize.width: root.coverSourceSize
        sourceSize.height: root.coverSourceSize
    }

    Kirigami.Icon {
        objectName: "albumArtPlaceholderGlyph"
        anchors.centerIn: parent
        width: root.placeholderGlyphSize
        height: width
        source: root.placeholderGlyphSource
        isMask: true
        color: root.placeholderGlyphColor
        visible: !root.hasCover
    }

    // The covers before fade out over this one, which shows whole all
    // along, so the panel never shows through between them. Each keeps its
    // slot while it fades, so its image is not loaded again.
    Item {
        anchors.fill: parent

        Repeater {
            model: root.outgoingSlots

            delegate: Rectangle {
                id: outgoing

                required property int index
                readonly property var cover: root.outgoingCovers[outgoing.index] || null
                readonly property string coverPath: outgoing.cover !== null ? outgoing.cover.coverPath : ""

                objectName: "albumArtOutgoing"
                // The older over the newer.
                z: -outgoing.index
                anchors.fill: parent
                visible: outgoing.cover !== null && outgoing.opacity > 0
                opacity: outgoing.cover !== null ? outgoing.cover.from * root.outgoingLevel : 0
                color: outgoing.coverPath.length > 0 ? root.backdropColor : root.placeholderColor
                // A cover fades out as one, on its backdrop: without a layer
                // the opacity would take the backdrop and the image apart,
                // and the backdrop would show through the image.
                layer.enabled: outgoing.visible && outgoing.coverPath.length > 0

                // The same size as the cover had, so the image cache still has it.
                Image {
                    anchors.fill: parent
                    source: outgoing.visible ? outgoing.coverPath : ""
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    asynchronous: true
                    cache: true
                    sourceSize.width: root.coverSourceSize
                    sourceSize.height: root.coverSourceSize
                }

                Kirigami.Icon {
                    anchors.centerIn: parent
                    width: root.placeholderGlyphSize
                    height: width
                    source: root.placeholderGlyphSource
                    isMask: true
                    color: root.placeholderGlyphColor
                    visible: outgoing.coverPath.length === 0
                }
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton

        onPressed: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                albumArtMenu.popup()
            }
        }

        onDoubleClicked: function(mouse) {
            if (mouse.button === Qt.LeftButton) {
                root.openAlbumArtViewer()
            }
        }
    }

    Menu {
        id: albumArtMenu

        MenuItem { action: root.replaceFromItunesAction }

        MenuItem {
            enabled: false
            visible: !root.replaceFromItunesAction.enabled
            text: root.currentTrackItunesArtworkDisabledReason()
        }
    }
}
