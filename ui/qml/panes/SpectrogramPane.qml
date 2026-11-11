// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Window 2.15

// The visualization's place in the main window: full bleed, on the plot's
// floor, with no border. Main.qml puts the visualization and its plot layer
// in `hostItem` while no viewer shows them.
//
// The viewer in the main window takes the visualization from here, and
// fades in over the pane as it opens: the pane keeps the visualization's
// last frame under it (#373 H5: it was empty for a few frames, so the
// opening viewer faded in over a blank pane). With `keepsLastFrame`, the
// pane grabs that frame once, while the visualization is still here, and
// `lastFrameKept` tells Main.qml it may take the visualization away.
Rectangle {
    id: root

    property alias hostItem: spectrogramMainHost
    required property var openViewer
    required property var uiPalette
    property int visualizationMode: 0
    // The visualization, whose last frame the pane keeps.
    property Item visualization: null
    // Whether the pane keeps the visualization's last frame, while a viewer
    // in this window shows it.
    property bool keepsLastFrame: false
    // Whether the frame is kept, so the visualization can go.
    readonly property bool lastFrameKept: root.keepsLastFrame && lastFrame.grabbed
    // Whether the pane can draw a frame to keep at all: hidden, empty or in
    // a window that does not draw, there is nothing to keep, and the
    // visualization goes at once.
    readonly property bool canKeepLastFrame: root.visible && root.width > 0 && root.height > 0
        && root.visualization !== null
        && root.Window.visibility !== Window.Hidden && root.Window.visibility !== Window.Minimized

    // The mode's floor, with or without data: the spectrogram's is its
    // colour map's silence, black; the waveform's the view colour.
    color: root.uiPalette.plotColors(root.visualizationMode).plotBg

    onKeepsLastFrameChanged: {
        lastFrame.grabbed = false
        if (root.keepsLastFrame) {
            lastFrame.keptSize = Qt.size(root.width, root.height)
            lastFrame.scheduleUpdate()
        }
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

    // The last frame: grabbed once, not live, so it costs nothing while it
    // shows. Under the host, where the visualization covers it until it
    // goes. Its size is the pane's when it was grabbed, whatever size the
    // visualization takes in the viewer.
    ShaderEffectSource {
        id: lastFrame
        objectName: "spectrogramPaneLastFrame"
        property bool grabbed: false
        property size keptSize: Qt.size(0, 0)
        width: lastFrame.keptSize.width
        height: lastFrame.keptSize.height
        visible: root.keepsLastFrame
        live: false
        hideSource: false
        sourceItem: root.keepsLastFrame ? root.visualization : null
        sourceRect: Qt.rect(0, 0, lastFrame.keptSize.width, lastFrame.keptSize.height)
        onScheduledUpdateCompleted: {
            if (root.keepsLastFrame) {
                lastFrame.grabbed = true
            }
        }
    }

    Item {
        id: spectrogramMainHost
        anchors.fill: parent
    }
}
