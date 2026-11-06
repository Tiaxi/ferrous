// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import "../components" as Components

Rectangle {
    id: root

    property alias hostItem: spectrogramMainHost
    required property var openViewer
    required property var uiPalette
    property int visualizationMode: 0
    property var setVisualizationMode: function(mode) {}

    // What the visualization keeps clear of in its top right corner: the
    // switch and the gap beside it.
    readonly property size reservedCorner: Qt.size(
        2 * modeSwitch.anchors.rightMargin + modeSwitch.width,
        modeSwitch.anchors.topMargin + modeSwitch.height)

    color: root.uiPalette.spectrogramPlot.plotBg
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
        id: modeSwitch
        z: 20
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        selectedMode: root.visualizationMode
        setMode: root.setVisualizationMode
    }
}
