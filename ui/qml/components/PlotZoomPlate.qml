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
    // following again; then noticeLeft. It comes as it goes, over 120 ms,
    // OutCubic, in either plot, while the plate shows. Zoomed, the plate
    // grows and narrows by its room at its left edge, Reset staying where
    // it is, and that room fades in and out as it does (its edge jumped 89
    // px in a frame with the row staying, H15, and stood as blank plate,
    // H16): coming, its text and divider fade in with the room, in their
    // place (the plate jumped 89 px wider in a frame, an empty box, while
    // they faded in, H17); going, its text is gone in the frame the view
    // comes back in, which it would contradict. At the default zoom, with
    // nothing else on it, the whole plate fades at its width, the text
    // with it (the text went in a frame while the empty plate faded on,
    // H16). A plate revealed with it comes at its width, its text fading
    // in on the plate's reveal. With the corner's row fading out as it
    // goes, as with the pointer off the pane, the notice stays on the
    // plate and goes with its fade, at its width, on the row's one curve:
    // the plate narrowed 15 ms ahead of the row's fade, its text gone at
    // full opacity (H17).
    property int notice: 0
    // Whether the corner's row stays as the notice goes, read as it starts
    // to go.
    property bool rowStays: true
    readonly property bool noticeShown: root.notice !== 0
    readonly property bool noticeLeaving: root.notice === 2
    signal noticeLeft()
    property string followShortcut: ""

    // How much of the notice is on the plate as it comes and goes, from 0
    // to 1, and what it does: 0 neither, 1 comes, 2 goes, 3 goes with the
    // row's fade. Its room, in
    // whole pixels: the notice's alone, or with the readout's 6 px from the
    // divider, more than its 6 px from the plate's edge.
    property real noticeLevel: 1
    property int _motion: 0
    readonly property bool _widening: root._motion === 1 && root.zoomed
    readonly property bool _narrowing: root._motion === 2 && root.zoomed
    readonly property real _room: followNotice.implicitWidth + (root.zoomed ? root.horizontalPadding : 0)
    readonly property real _roomLeft: root.noticeLevel >= 1 ? root._room
        : Math.round(root._room * root.noticeLevel)
    // As the room grows, the notice in it, less the readout's 4 px more on
    // its side of the divider: with no room for it, the row skips it.
    readonly property real _slot: Math.max(0, root._roomLeft - root.horizontalPadding)
    readonly property bool _noticeInRow: (root.notice === 1 && !(root._widening && root._slot <= 0))
        || (root.notice === 2 && (!root.zoomed || root._motion === 3))
    // The text's and the divider's own fade, as a plate is revealed with
    // them (they stood on it in a frame as it faded in, H15).
    property real _textLevel: 1
    onNoticeChanged: {
        noticeMotion.stop()
        textArrival.stop()
        root._textLevel = 1
        if (root.notice === 1) {
            // From what is left of it as it went, or from none.
            const from = root._motion === 2 ? root.noticeLevel : 0
            if (from < 1 && Kirigami.Units.shortDuration > 0 && root.available
                    && root.revealLevel * root.fadeLevel > 0) {
                root._motion = 1
                root.noticeLevel = from
                noticeMotion.to = 1
                noticeMotion.start()
            } else {
                root._motion = 0
                root.noticeLevel = 1
                if (from <= 0 && Kirigami.Units.shortDuration > 0) {
                    root._textLevel = 0
                    textArrival.start()
                }
            }
        } else if (root.notice === 2) {
            if (root.rowStays || root._motion === 1) {
                // From all of it, or from what came of it.
                root._leaveAlone()
            } else {
                root._motion = 3
                root.noticeLevel = 1
                root._goneWithRow()
            }
        } else {
            root._motion = 0
            root.noticeLevel = 1
        }
    }
    function _leaveAlone() {
        noticeMotion.stop()
        root._motion = 2
        noticeMotion.to = 0
        noticeMotion.start()
    }
    // Gone with the row as it has faded out; going alone as the row comes
    // back first.
    function _goneWithRow() {
        if (root._motion === 3 && (!root.available || root.revealLevel * root.fadeLevel <= 0))
            root.noticeLeft()
    }
    onRevealLevelChanged: root._goneWithRow()
    onFadeLevelChanged: root._goneWithRow()
    onAvailableChanged: root._goneWithRow()
    onRevealedChanged: {
        if (root._motion === 3 && root.revealed)
            root._leaveAlone()
    }
    NumberAnimation {
        id: noticeMotion
        target: root
        property: "noticeLevel"
        duration: Math.round(Kirigami.Units.shortDuration * 1.2)
        easing.type: Easing.OutCubic
        onFinished: {
            if (root._motion === 2)
                root.noticeLeft()
            else
                root._motion = 0
        }
    }
    NumberAnimation {
        id: textArrival
        target: root
        property: "_textLevel"
        from: 0
        to: 1
        duration: Kirigami.Units.shortDuration
        easing.type: Easing.OutCubic
    }

    visible: root.available && (root.zoomed || root.noticeShown)
    opacity: root.revealLevel * root.fadeLevel
        * ((root._motion === 1 || root._motion === 2) && !root.zoomed ? root.noticeLevel : 1)
    // As it grows and narrows, its fill and edge are the parts below.
    bare: root._widening || root._narrowing
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

    // The plate as it grows and narrows, cut where the notice's room ends:
    // that room, fading in and out, and the rest, the plate as it is
    // without the notice, so the two meet as one plate with the room
    // whole. The rest's left corners round as the room goes, its edge
    // there coming in with them (they were cut square, H17), and the
    // room's edge reaches under them, for the plate's edge to run on as
    // the room comes and goes. Behind the readout and Reset.
    readonly property real _edgeCorner: root.radius + 1
    Item {
        objectName: "plotZoomPlateRoom"
        parent: root
        z: -1
        visible: root.bare
        opacity: root.noticeLevel
        Rectangle {
            x: 0
            width: root._roomLeft
            height: root.height
            radius: root.radius
            topRightRadius: 0
            bottomRightRadius: 0
            color: root.plotColors.plotPlate
        }
        Item {
            x: -1
            y: -1
            width: root._roomLeft + root._edgeCorner
            height: root.height + 2
            clip: true
            Rectangle {
                width: root.width + 2
                height: parent.height
                radius: root._edgeCorner
                color: "transparent"
                border.width: 1
                border.color: root.plotColors.plotPlateEdge
            }
        }
    }
    Item {
        objectName: "plotZoomPlateStaying"
        parent: root
        z: -1
        visible: root.bare
        x: root._roomLeft
        width: root.width - root._roomLeft
        height: root.height
        readonly property real cornerLevel: 1 - root.noticeLevel
        Rectangle {
            anchors.fill: parent
            radius: root.radius
            topLeftRadius: root.radius * parent.cornerLevel
            bottomLeftRadius: root.radius * parent.cornerLevel
            color: root.plotColors.plotPlate
        }
        // Its edge: the left of it, with its corners, coming in as the
        // room goes, and the rest, as the plate's.
        Item {
            objectName: "plotZoomPlateStayingEdge"
            x: -1
            y: -1
            width: root._edgeCorner
            height: parent.height + 2
            clip: true
            opacity: parent.cornerLevel
            Rectangle {
                width: parent.parent.width + 2
                height: parent.height
                radius: root._edgeCorner
                topLeftRadius: root._edgeCorner * parent.parent.cornerLevel
                bottomLeftRadius: root._edgeCorner * parent.parent.cornerLevel
                color: "transparent"
                border.width: 1
                border.color: root.plotColors.plotPlateEdge
            }
        }
        Item {
            x: root._edgeCorner - 1
            y: -1
            width: Math.max(0, parent.width + 2 - root._edgeCorner)
            height: parent.height + 2
            clip: true
            Rectangle {
                x: -root._edgeCorner
                width: parent.parent.width + 2
                height: parent.height
                radius: root._edgeCorner
                color: "transparent"
                border.width: 1
                border.color: root.plotColors.plotPlateEdge
            }
        }
    }

    // As it comes, in its room, in its place, which the room's edge shows
    // as it grows; as it goes, zoomed, the readout keeps the room.
    Components.FollowNotice {
        id: followNotice
        objectName: "plotFollowNotice"
        // From the state and the way it goes: read with the plate's
        // narrowing, which ends with the state, it showed for an instant as
        // that ended, and its text started to come in again behind the
        // hidden plate (H17).
        visible: root.notice === 1 || (root.notice === 2 && (!root.zoomed || root._motion === 3))
        width: root._widening ? root._slot : implicitWidth
        clip: root._widening
        contentShift: root._widening ? root._slot - implicitWidth : 0
        arrivalLevel: root._widening ? root.noticeLevel : root._textLevel
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
        leftPadding: root._noticeInRow ? 6 - root.contentSpacing
            : 6 - root.horizontalPadding + (root.bare ? root._roomLeft : 0)
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
