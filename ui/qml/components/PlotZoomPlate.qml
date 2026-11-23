// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// The plot's zoom while it is zoomed, in figures, which nothing else has
// with the scale off: a readout and Reset, on a plate in the plot's colours, left of
// the mode switch in the main pane, as tall as the switch, with its 4 px of
// plate around the buttons (PlotModeSwitch); a viewer's are in its own
// controls (ViewerControls). Set `revealed` as the switch's: faded out,
// Reset stays a Tab stop, which reveals it. Not zoomed, it is hidden and
// no Tab stop, but for a moved view's "Not following" (FollowNotice),
// which comes before the zoom or stands alone.
Components.PlotPlate {
    id: root
    objectName: "plotZoomPlate"
    padding: 4

    // The view's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    property var pointerActivity: null
    // Whether it may show at all: not while a viewer's controls are hidden.
    property bool available: true
    readonly property bool zoomed: Math.abs(root.zoomLevel - 1.0) > 0.001
    // Whether it says that the view does not follow the playhead.
    property bool noticeShown: false
    // Whether the notice is leaving as the row fades out, the view
    // following again: its text is gone at once, its room kept.
    property bool noticeLeaving: false
    property string followShortcut: ""

    visible: root.available && (root.zoomed || root.noticeShown)
    takesPointer: true
    Accessible.name: root.zoomed
        ? I18n.i18nc("@label", "Zoom")
        : I18n.i18nc("@label the corner of the visualization that tells of its view", "View")

    readonly property bool pointerHovered: zoomHover.hovered
    // Whether keyboard focus is on Reset.
    readonly property bool focusInside: resetButton.activeFocus

    HoverHandler {
        id: zoomHover
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    Components.FollowNotice {
        objectName: "plotFollowNotice"
        visible: root.noticeShown
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        shortcut: root.followShortcut
        divided: root.zoomed
        textShown: !root.noticeLeaving
        inset: 6 - root.horizontalPadding
    }

    // The readout 6 px in from the plate's edge, as a lane's label is on
    // its plate, and 6 px from the box of Reset's glyph, which leaves about
    // 8 px between their ink.
    Components.ZoomReadout {
        objectName: "plotZoomReadout"
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        zoomLevel: root.zoomLevel
        // Only the notice alone takes their room: while the plate is hidden
        // they keep it, so that it has its size as it appears, a row
        // placing what just appeared only when the window next draws.
        visible: root.zoomed || !root.noticeShown
        // 6 px from the notice's divider too.
        leftPadding: 6 - (root.noticeShown ? root.contentSpacing : root.horizontalPadding)
        rightPadding: 0
    }

    Components.PlotToolButton {
        id: resetButton
        objectName: "plotZoomResetButton"
        visible: (root.zoomed || !root.noticeShown) && root.resetView !== null
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: I18n.i18nc("@info:tooltip", "Reset Zoom")
        icon.name: "zoom-original"
        onClicked: root.resetView()
    }
}
