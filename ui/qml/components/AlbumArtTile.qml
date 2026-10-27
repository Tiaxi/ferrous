// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

Rectangle {
    id: root

    required property var uiBridge
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer

    implicitWidth: 320
    implicitHeight: 320
    Layout.fillWidth: true
    Layout.preferredHeight: width

    color: Kirigami.Theme.alternateBackgroundColor
    Accessible.role: Accessible.Graphic
    Accessible.name: root.uiBridge.currentTrackCoverPath.length > 0
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

    Label {
        anchors.centerIn: parent
        text: I18n.i18nc("@label", "Album Art")
        color: Kirigami.Theme.disabledTextColor
        visible: root.uiBridge.currentTrackCoverPath.length === 0
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
