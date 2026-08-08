// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import "../components" as Components

Rectangle {
    id: root

    property alias hostItem: spectrogramMainHost
    required property var openViewer
    property int visualizationMode: 0
    property var setVisualizationMode: function(mode) {}

    color: "#0b0b0f"
    border.color: Qt.rgba(0, 0, 0, 0.25)

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onDoubleClicked: function(mouse) {
            if (mouse.button === Qt.LeftButton) {
                root.openViewer()
            }
        }
    }

    Item {
        id: spectrogramMainHost
        anchors.fill: parent
    }

    Components.VisualizationModeSwitch {
        z: 20
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        selectedMode: root.visualizationMode
        setMode: root.setVisualizationMode
    }
}
