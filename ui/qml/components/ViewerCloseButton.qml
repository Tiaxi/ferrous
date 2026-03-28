// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: root

    property color fillColor: Qt.rgba(0, 0, 0, 0.45)
    property color borderColor: Qt.rgba(1, 1, 1, 0.24)
    property color iconColor: "white"
    signal clicked()

    width: 40
    height: 40
    radius: 8
    color: root.fillColor
    border.color: root.borderColor

    ToolButton {
        anchors.fill: parent
        icon.name: "window-close"
        icon.color: root.iconColor
        onClicked: root.clicked()
    }
}
