// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// The plot's zoom while it is zoomed, which nothing else shows with the
// scale off: a readout and Reset, on a plate in the plot's colours, left of
// the mode switch in the main pane and in both viewers. Not zoomed, it is
// hidden and no Tab stop.
Components.PlotPlate {
    id: root
    objectName: "plotZoomPlate"

    // The view's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    property var pointerActivity: null
    // Whether it may show at all: not while a viewer's controls are hidden.
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

    // The readout 6 px in from the plate's edge, as a lane's label is on
    // its plate, and 6 px from the box of Reset's glyph, which leaves about
    // 8 px between their ink (#373 H6: 8 px and 10 px).
    Components.ZoomReadout {
        objectName: "plotZoomReadout"
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        zoomLevel: root.zoomLevel
        leftPadding: 6 - root.horizontalPadding
        rightPadding: 0
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
