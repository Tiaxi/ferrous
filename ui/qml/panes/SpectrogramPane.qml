// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

Rectangle {
    id: root

    property alias hostItem: spectrogramMainHost
    required property var openViewer

    color: "#0b0b0f"
    border.color: Qt.rgba(0, 0, 0, 0.25)

    Item {
        id: spectrogramMainHost
        anchors.fill: parent
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onDoubleClicked: function(mouse) {
            if (mouse.button === Qt.LeftButton) {
                root.openViewer()
            }
        }
    }
}
