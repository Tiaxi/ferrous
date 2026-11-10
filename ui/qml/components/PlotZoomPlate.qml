// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// The main pane's zoom while the plot is zoomed, which nothing else there
// shows with the scale off: the viewers' readout and Reset, on a plate in
// the plot's colours. Not zoomed, it is hidden and no Tab stop.
Components.PlotPlate {
    id: root
    objectName: "plotZoomPlate"

    // The view's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    property var pointerActivity: null
    // Whether it may show at all: a viewer shows the zoom in its own
    // controls.
    property bool available: true
    readonly property bool zoomed: Math.abs(root.zoomLevel - 1.0) > 0.001

    visible: root.available && root.zoomed
    takesPointer: true
    Accessible.name: I18n.i18nc("@label", "Zoom")

    HoverHandler {
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    Components.ZoomReadout {
        objectName: "plotZoomReadout"
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        zoomLevel: root.zoomLevel
        leftPadding: 4
        rightPadding: 2
    }

    Components.PlotToolButton {
        objectName: "plotZoomResetButton"
        visible: root.resetView !== null
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: I18n.i18nc("@info:tooltip", "Reset Zoom")
        icon.name: "zoom-original"
        onClicked: root.resetView()
    }
}
