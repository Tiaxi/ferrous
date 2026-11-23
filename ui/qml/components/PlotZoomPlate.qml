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
    // "Not following" (FollowNotice) on the plate, from the one state its
    // view tells, so that what follows from it never reads one part of it
    // changed and another not yet: 0 none, 1 shown, 2 leaving, the view
    // following again, over 120 ms, OutCubic, whether the row stays or
    // fades out with it, and in either plot; then noticeLeft. Zoomed, its
    // text is gone in the frame the view comes back in, and the plate
    // narrows to its width without it at its left edge, Reset staying
    // where it is (its edge jumped 89 px in a frame with the row staying,
    // H15): the room it gives up fades as it narrows, rather than stand as
    // blank plate (H16). At the default zoom, with nothing else on it, the
    // whole plate fades at its width, the text with it: the text went in a
    // frame while the empty plate faded on (H16).
    property int notice: 0
    readonly property bool noticeShown: root.notice !== 0
    readonly property bool noticeLeaving: root.notice === 2
    readonly property bool _narrowing: root.noticeLeaving && root.zoomed
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
            root._leavingRoom = followNotice.implicitWidth + (root.zoomed ? root.horizontalPadding : 0)
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
    // As it narrows, its fill and edge are the two parts below.
    bare: root._narrowing
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

    // The plate as it narrows, cut where the room the notice leaves ends:
    // that room, fading as it narrows, and the rest, as the plate is. Each
    // is the whole plate's fill and edge, clipped to its part, so the two
    // meet as one plate in the frame the view comes back in, and the rest
    // is the whole plate, its left corners rounded, as the room closes.
    // Behind the readout and Reset.
    Item {
        objectName: "plotZoomPlateLeavingRoom"
        parent: root
        z: -1
        visible: root._narrowing
        opacity: root.leavingLevel
        x: -1
        y: -1
        width: Math.max(0, root._roomLeft)
        height: root.height + 2
        clip: true
        Rectangle {
            x: 0
            width: root.width + 2
            height: parent.height
            radius: root.radius + 1
            color: "transparent"
            border.width: 1
            border.color: root.plotColors.plotPlateEdge
        }
        Rectangle {
            x: 1
            y: 1
            width: root.width
            height: root.height
            radius: root.radius
            color: root.plotColors.plotPlate
        }
    }
    Item {
        objectName: "plotZoomPlateStaying"
        parent: root
        z: -1
        visible: root._narrowing
        x: root._roomLeft - 1
        y: -1
        width: Math.max(0, root.width - root._roomLeft + 2)
        height: root.height + 2
        clip: true
        Rectangle {
            x: -root._roomLeft
            width: root.width + 2
            height: parent.height
            radius: root.radius + 1
            color: "transparent"
            border.width: 1
            border.color: root.plotColors.plotPlateEdge
        }
        Rectangle {
            x: 1 - root._roomLeft
            y: 1
            width: root.width
            height: root.height
            radius: root.radius
            color: root.plotColors.plotPlate
        }
        // Its own left edge, between its corners, coming in as the room
        // goes: the light plot shows the plate by its edge.
        Rectangle {
            objectName: "plotZoomPlateStayingEdge"
            x: 0
            y: 1 + root.radius
            width: 1
            height: Math.max(0, root.height - 2 * root.radius)
            opacity: 1 - root.leavingLevel
            color: root.plotColors.plotPlateEdge
        }
    }

    // As it leaves, the readout keeps its room while the plate is zoomed,
    // and it keeps its own otherwise.
    Components.FollowNotice {
        id: followNotice
        objectName: "plotFollowNotice"
        // From the state alone: read with the plate's narrowing, which
        // ends with the state, it showed for an instant as that ended, and
        // its text started to come in again behind the hidden plate (H17).
        shown: root.notice === 1 || (root.notice === 2 && !root.zoomed)
        width: implicitWidth
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        shortcut: root.followShortcut
        divided: root.zoomed
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
