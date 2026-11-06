// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

// The playing track's cover, edge to edge: no inset, radius or frame. A
// track without a cover shows a faint tile with the album glyph.
Rectangle {
    id: root

    required property var uiBridge
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer
    // The tile and its glyph while the track has no cover: the now-playing
    // panel passes the palette's.
    property color placeholderColor: "transparent"
    property color placeholderGlyphColor: Kirigami.Theme.textColor
    readonly property bool hasCover: root.uiBridge.currentTrackCoverPath.length > 0

    implicitWidth: 320
    implicitHeight: 320
    Layout.fillWidth: true
    Layout.preferredHeight: width

    color: root.hasCover ? "transparent" : root.placeholderColor
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
        sourceSize.width: Math.max(256, width)
        sourceSize.height: Math.max(256, height)
    }

    Kirigami.Icon {
        objectName: "albumArtPlaceholderGlyph"
        anchors.centerIn: parent
        width: Math.round(Math.min(parent.width, parent.height) / 3)
        height: width
        source: "media-album-track"
        isMask: true
        color: root.placeholderGlyphColor
        visible: !root.hasCover
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
