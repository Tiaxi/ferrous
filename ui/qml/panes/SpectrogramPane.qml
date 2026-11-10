// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

// The visualization's place in the main window: full bleed, on the plot's
// floor, with no border. Main.qml puts the visualization and its plot layer
// in `hostItem` while no viewer shows them.
Rectangle {
    id: root

    property alias hostItem: spectrogramMainHost
    required property var openViewer
    required property var uiPalette
    property int visualizationMode: 0
    // Before the session's first audio data: the light theme's bare pane is
    // the view colour, as PlotLayer draws it over the plot.
    property bool awaitingFirstData: false

    color: root.awaitingFirstData && !root.uiPalette.themeIsDark
        ? root.uiPalette.view
        : root.uiPalette.plotColors(root.visualizationMode).plotBg

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
}
