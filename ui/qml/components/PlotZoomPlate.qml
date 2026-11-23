// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import org.kde.kirigami 2.20 as Kirigami
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
    // Whether the notice is leaving, the view following again: its text
    // is gone in the frame the view comes back in, and the plate narrows
    // from the width it had to its new one over 120 ms, OutCubic, at its
    // left edge, Reset staying where it is; with nothing left on it, it
    // fades as it narrows. Whether the row stays or fades out with it, and
    // in either plot, it goes this one way (the plate's edge jumped 89 px
    // in a frame with the row staying). Then noticeLeft.
    property bool noticeLeaving: false
    signal noticeLeft()
    property string followShortcut: ""

    // How much of the notice's room is left as it leaves, from 1 to 0, and
    // that room, in whole pixels: the notice's alone, or with the readout's
    // 6 px from the divider, more than its 6 px from the plate's edge.
    property real leavingLevel: 0
    property real _leavingRoom: 0
    readonly property real _roomLeft: root.leavingLevel >= 1 ? root._leavingRoom
        : Math.round(root._leavingRoom * root.leavingLevel)
    onNoticeLeavingChanged: {
        leavingMotion.stop()
        if (root.noticeLeaving) {
            root._leavingRoom = notice.implicitWidth + (root.zoomed ? root.horizontalPadding : 0)
            root.leavingLevel = 1
            leavingMotion.start()
        } else {
            root.leavingLevel = 0
        }
    }
    NumberAnimation {
        id: leavingMotion
        target: root
        property: "leavingLevel"
        from: 1
        to: 0
        duration: Math.round(Kirigami.Units.shortDuration * 1.2)
        easing.type: Easing.OutCubic
        onFinished: root.noticeLeft()
    }

    visible: root.available && (root.zoomed || root.noticeShown)
    opacity: root.revealLevel * root.fadeLevel
        * (root.noticeLeaving && !root.zoomed ? root.leavingLevel : 1)
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

    // As it leaves, the readout keeps its room while the plate is zoomed,
    // and it keeps its own otherwise.
    Components.FollowNotice {
        id: notice
        objectName: "plotFollowNotice"
        shown: root.noticeShown && !(root.noticeLeaving && root.zoomed)
        width: root.noticeLeaving ? root._roomLeft : implicitWidth
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
        // 6 px from the notice's divider too, and the room it leaves.
        leftPadding: 6 - (root.noticeShown && !root.noticeLeaving ? root.contentSpacing : root.horizontalPadding)
            + (root.noticeLeaving ? root._roomLeft : 0)
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
