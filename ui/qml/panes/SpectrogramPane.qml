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
//
// The frame is a ShaderEffectSource of the visualization, which Qt keeps
// in the effect's window for as long as the effect refers to it, whatever
// window its parent is in: taken into the whole-screen window then, it
// stayed in this window's scene, and the two windows' render threads drew
// its nodes at once (#373 H6: a crash). So the effect is made only while
// the pane keeps a frame, and only once the visualization is here; it lets
// the visualization go and is destroyed as soon as the pane keeps none,
// and `holdsVisualization` tells Main.qml not to take the visualization
// into another window until it has.
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
    readonly property bool lastFrameKept: root.keepsLastFrame && lastFrameLoader.item !== null
        && lastFrameLoader.item.grabbed
    // Whether the kept frame refers to the visualization, which then stays
    // in this window.
    readonly property bool holdsVisualization: lastFrameLoader.item !== null
    // Whether the pane can draw a frame to keep at all: hidden, empty or in
    // a window that does not draw, there is nothing to keep, and the
    // visualization goes at once.
    readonly property bool canKeepLastFrame: root.visible && root.width > 0 && root.height > 0
        && root.visualization !== null
        && root.Window.visibility !== Window.Hidden && root.Window.visibility !== Window.Minimized

    // The mode's floor, with or without data: the spectrogram's is its
    // colour map's silence, black; the waveform's the view colour.
    color: root.uiPalette.plotColors(root.visualizationMode).plotBg

    onKeepsLastFrameChanged: root.updateLastFrame()

    Connections {
        target: root.visualization
        function onParentChanged() { root.updateLastFrame() }
    }

    // Makes the frame once the pane is to keep one and the visualization is
    // here to grab, and lets the visualization go and destroys the frame as
    // soon as it is not to. The effect takes its source only while it is in
    // this window, and gives it up before it leaves: Qt counts the window
    // an effect without one refers to, and does not count it off again.
    function updateLastFrame() {
        if (!root.keepsLastFrame) {
            if (lastFrameLoader.item !== null) {
                lastFrameLoader.item.sourceItem = null
            }
            lastFrameLoader.active = false
            return
        }
        if (lastFrameLoader.active || root.visualization === null
                || root.visualization.parent !== spectrogramMainHost) {
            return
        }
        lastFrameLoader.active = true
        const frame = lastFrameLoader.item
        frame.keptSize = Qt.size(root.width, root.height)
        frame.sourceItem = root.visualization
        frame.scheduleUpdate()
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
    Loader {
        id: lastFrameLoader
        active: false
        sourceComponent: ShaderEffectSource {
            id: lastFrame
            objectName: "spectrogramPaneLastFrame"
            property bool grabbed: false
            property size keptSize: Qt.size(0, 0)
            width: lastFrame.keptSize.width
            height: lastFrame.keptSize.height
            live: false
            hideSource: false
            sourceRect: Qt.rect(0, 0, lastFrame.keptSize.width, lastFrame.keptSize.height)
            onScheduledUpdateCompleted: lastFrame.grabbed = lastFrame.sourceItem !== null
        }
    }

    Item {
        id: spectrogramMainHost
        anchors.fill: parent
    }
}
