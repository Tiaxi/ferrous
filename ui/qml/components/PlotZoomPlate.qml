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
    // following again; then noticeLeft. It comes and goes while the plate
    // shows, in either plot, on one plate with its own fill and edge, its
    // corners round: the plate split in two, a room for the notice fading
    // beside the rest, read as two tones with a seam (H18). Zoomed, the
    // plate grows and narrows at its left edge, Reset staying where it is,
    // and the text and divider stand in their place, drawn only while the
    // plate holds them: clipped to the growing plate, the text was cut
    // mid-letter for 83 ms (H18). Coming, the plate grows over 50 ms,
    // OutCubic, and the text and divider fade in whole over 60 ms, OutCubic,
    // as soon as it holds them, about 32 ms in, so that the plate and its
    // text arrive as one (grown over 80 ms, it stood empty for 57 ms, H20);
    // going, they fade out over 40 ms, OutCubic, as the
    // plate narrows over 90 ms, InOutCubic, slowly at first, so they are
    // gone before it is narrower than they are: gone in a frame, an empty
    // box narrowed for 60 ms (H18). At the default zoom, with nothing else
    // on it, the whole plate fades at its width over 120 ms, OutCubic, the
    // text with it (the text went in a frame while the empty plate faded
    // on, H16). A plate revealed with it comes at its width, its text
    // fading in on the plate's reveal. With the corner's row fading out as
    // it goes, as with the pointer off the pane, its view sets it to 0 with
    // the view's cut (PlotLayer): riding the row's fade, "Not following"
    // was still legible 19 ms after the view had come back (H22), and with
    // its words gone at the cut, the plate faded at its width, its room
    // empty (H23).
    property int notice: 0
    readonly property bool noticeShown: root.notice !== 0
    readonly property bool noticeLeaving: root.notice === 2
    signal noticeLeft()
    property string followShortcut: ""

    // How much of the notice is on the plate as it comes and goes, from 0
    // to 1, and what it does: 0 neither, 1 comes, 2 goes. Zoomed, it is the plate's width; at the default zoom,
    // its opacity.
    property real noticeLevel: 1
    property int _motion: 0
    // The notice's room, in whole pixels: the notice's alone, or with the
    // readout's 6 px from the divider, more than its 6 px from the plate's
    // edge; and how much of it the plate has.
    readonly property real _room: followNotice.implicitWidth + (root.zoomed ? root.horizontalPadding : 0)
    readonly property real _roomLeft: !root.zoomed || root.noticeLevel >= 1 ? root._room
        : Math.round(root._room * root.noticeLevel)
    // The notice's box in the row, less the readout's 4 px more on its
    // side of the divider: with no room for it, the row skips it.
    readonly property real _slot: root.zoomed ? Math.max(0, root._roomLeft - root.horizontalPadding)
        : followNotice.implicitWidth
    // Where its text starts in the plate, 6 px in from the plate's edge as
    // the plate is whole, and whether the plate holds it, 2 px in or more.
    readonly property real _textLeft: root._roomLeft - root._room + 6
    readonly property bool _holdsText: root._textLeft >= 2
    // The text's and the divider's own level, as they come and go on a
    // plate that shows, and as a plate is revealed with them (they stood on
    // it in a frame as it faded in, H15).
    property real _textLevel: 1
    onNoticeChanged: {
        noticeMotion.stop()
        textMotion.stop()
        const animated = Kirigami.Units.shortDuration > 0
        if (root.notice === 1) {
            // From what is left of it as it went, or from none.
            const from = root._motion === 2 ? root.noticeLevel : 0
            const textFrom = root._motion === 2 ? root._textLevel : 0
            if (from < 1 && animated && root.available && root.revealLevel * root.fadeLevel > 0) {
                root._motion = 1
                root.noticeLevel = from
                if (root.zoomed) {
                    root._textLevel = textFrom
                    root._move(1, 0.5, Easing.OutCubic)
                    root._fadeText(1, root._holdingTime(from, 0.5), 0.6)
                } else {
                    root._textLevel = 1
                    root._move(1, 1.2, Easing.OutCubic)
                }
            } else {
                root._motion = 0
                root.noticeLevel = 1
                root._textLevel = from <= 0 && animated ? 0 : 1
                if (root._textLevel < 1)
                    root._fadeText(1, 0, 1)
            }
        } else if (root.notice === 2) {
            // From all of it, or from what came of it.
            root._leaveAlone()
        } else {
            root._motion = 0
            root.noticeLevel = 1
            root._textLevel = 1
        }
    }
    function _leaveAlone() {
        noticeMotion.stop()
        textMotion.stop()
        root._motion = 2
        if (root.zoomed) {
            root._move(0, 0.9, Easing.InOutCubic)
            root._fadeText(0, 0, 0.4)
        } else {
            root._move(0, 1.2, Easing.OutCubic)
        }
    }
    // The notice's level to `to`, over `scale` of shortDuration.
    function _move(to, scale, easing) {
        noticeMotion.to = to
        noticeMotion.duration = Math.round(Kirigami.Units.shortDuration * scale)
        noticeMotion.easing.type = easing
        noticeMotion.start()
    }
    // The text's level, from where it is, to `to` after `pause` ms, over
    // `scale` of shortDuration, OutCubic.
    function _fadeText(to, pause, scale) {
        textPause.duration = Math.max(0, Math.round(pause))
        textFade.to = to
        textFade.duration = Math.round(Kirigami.Units.shortDuration * scale)
        textMotion.start()
    }
    // How long, in ms, the plate growing from `from` over `scale` of
    // shortDuration, OutCubic, takes to hold the text.
    function _holdingTime(from, scale) {
        const hold = (root._room - 4) / root._room
        const part = (hold - from) / (1 - from)
        if (part <= 0)
            return 0
        return Kirigami.Units.shortDuration * scale * (1 - Math.cbrt(1 - Math.min(1, part)))
    }
    NumberAnimation {
        id: noticeMotion
        target: root
        property: "noticeLevel"
        onFinished: {
            if (root._motion === 2)
                root.noticeLeft()
            else
                root._motion = 0
        }
    }
    SequentialAnimation {
        id: textMotion
        PauseAnimation {
            id: textPause
        }
        NumberAnimation {
            id: textFade
            target: root
            property: "_textLevel"
            easing.type: Easing.OutCubic
        }
    }

    // With nothing but the zoom on it, the plate comes on the first wheel
    // notch and goes on Reset as the corner's other plates come and go,
    // fading over shortDuration, OutCubic, where it cut in and out in a
    // frame (H22). Only with the corner revealed and staying; with the notice
    // on the plate, which keeps it, the notice's own motion is the plate's.
    // Going, it holds the last zoom it had, never "Zoom 100%".
    property real _zoomPresence: 1
    property real _lastZoom: 1.0
    onZoomLevelChanged: {
        if (Math.abs(root.zoomLevel - 1.0) > 0.001)
            root._lastZoom = root.zoomLevel
    }
    onZoomedChanged: {
        const wasMoving = zoomPresence.running
        zoomPresence.stop()
        if (!root.noticeShown && Kirigami.Units.shortDuration > 0 && root.available
                && root.revealed && root.revealLevel * root.fadeLevel >= 1) {
            // From what shows of it, as it was coming or going, or from none.
            if (root.zoomed && !wasMoving)
                root._zoomPresence = 0
            zoomPresence.to = root.zoomed ? 1 : 0
            zoomPresence.start()
        } else {
            root._zoomPresence = 1
        }
    }
    NumberAnimation {
        id: zoomPresence
        target: root
        property: "_zoomPresence"
        duration: Kirigami.Units.shortDuration
        easing.type: Easing.OutCubic
        // Gone: hidden, and whole for its next showing.
        onFinished: root._zoomPresence = 1
    }
    readonly property bool _zoomLeaving: zoomPresence.running && !root.zoomed

    visible: root.available && (root.zoomed || root.noticeShown || root._zoomLeaving)
    opacity: root.revealLevel * root.fadeLevel
        * ((root._motion === 1 || root._motion === 2) && !root.zoomed ? root.noticeLevel : 1)
        * (root.noticeShown ? 1 : root._zoomPresence)
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

    // In its room, as much of it as the plate has, its text and divider in
    // their place at the room's right end.
    Components.FollowNotice {
        id: followNotice
        objectName: "plotFollowNotice"
        visible: root.noticeShown && root._slot > 0
        width: root._slot
        arrivalLevel: root._holdsText ? root._textLevel : 0
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
        zoomLevel: root._zoomLeaving ? root._lastZoom : root.zoomLevel
        // Only the notice alone takes their room: while the plate is hidden
        // they keep it, so that it has its size as it appears, a row
        // placing what just appeared only when the window next draws.
        visible: root.zoomed || !root.noticeShown
        // 6 px from the notice's divider too, and the room it leaves.
        leftPadding: followNotice.visible ? 6 - root.contentSpacing
            : 6 - root.horizontalPadding + (root.zoomed && root.noticeShown ? root._roomLeft : 0)
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
