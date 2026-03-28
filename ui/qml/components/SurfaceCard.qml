// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

Rectangle {
    id: root

    property color borderColor: "transparent"
    property int padding: 0
    default property alias contentData: contentHost.data

    radius: 10
    border.color: root.borderColor
    implicitWidth: Math.max(0, contentHost.childrenRect.width + (padding * 2))
    implicitHeight: Math.max(0, contentHost.childrenRect.height + (padding * 2))

    Item {
        id: contentHost
        anchors.fill: parent
        anchors.margins: root.padding
    }
}
