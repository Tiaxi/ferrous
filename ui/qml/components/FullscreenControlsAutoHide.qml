// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami

QtObject {
    id: root

    property bool active: false
    property int hideDelay: 5000
    // Keeps the controls shown, such as while one has keyboard focus.
    property bool held: false
    // A button down and moved on the view: a drag is pointer activity for
    // as long as it lasts, paused in it or not (the hover that counts as
    // activity stops while a button is down, and a pan longer than the delay
    // lost its hand and its crosshair). A press that moves nothing is none.
    property bool dragging: false
    readonly property bool controlsVisible: !root.active || root.held || root.dragging
        || state.controlsVisible
    // The controls as they fade, 1 shown and 0 hidden: every overlay that
    // hides with them, the title strip, the corner controls, the mode
    // switch, the lanes' Mute and Solo and the crosshair, takes its opacity
    // from this one level, so none of them cuts while another fades. They
    // come at once, over shortDuration on OutCubic, as they answer the
    // pointer; nobody asks for them to go, and they leave as a fade, 2.5
    // times as long (250 ms), on InOutQuad (on the one quick curve both
    // ways they were gone in three frames, a pop along the screen's top
    // every time the pointer rested). They take no pointer only once it is
    // 0, and the pointer hides then too.
    property real level: root.controlsVisible ? 1 : 0
    // Kirigami's durations are 0 with animations turned off.
    Behavior on level {
        id: levelFade
        enabled: Kirigami.Units.shortDuration > 0
        NumberAnimation {
            // By where the fade goes: controlsVisible may not have told
            // this binding yet as the fade starts.
            readonly property bool shows: levelFade.targetValue > 0
            duration: shows ? Kirigami.Units.shortDuration : Math.round(Kirigami.Units.shortDuration * 2.5)
            easing.type: shows ? Easing.OutCubic : Easing.InOutQuad
        }
    }

    function pointerMoved(x, y) {
        if (!root.active) {
            return
        }
        if (state.hasPointerPosition
                && state.pointerX === x
                && state.pointerY === y) {
            return
        }
        state.hasPointerPosition = true
        state.pointerX = x
        state.pointerY = y
        state.controlsVisible = true
        hideTimer.restart()
    }

    // Follows a press on the viewer, from a PointHandler's `active` and
    // point: it drags once it has moved as far as a drag starts.
    function pressTracked(pressed, position, pressPosition) {
        if (!pressed || !root.active) {
            root.dragging = false
            return
        }
        const threshold = Application.styleHints.startDragDistance
        if (root.dragging || Math.abs(position.x - pressPosition.x) >= threshold
                || Math.abs(position.y - pressPosition.y) >= threshold) {
            root.dragging = true
            root.pointerMoved(position.x, position.y)
        }
    }

    // Shows the controls for a key press, as a pointer move does, so the
    // keyboard reaches them.
    function wake() {
        if (!root.active) {
            return
        }
        state.controlsVisible = true
        hideTimer.restart()
    }

    function reset() {
        hideTimer.stop()
        state.controlsVisible = true
        state.hasPointerPosition = false
        if (root.active) {
            hideTimer.start()
        }
    }

    // Hidden no sooner than the delay after a control lets focus go.
    onHeldChanged: {
        if (!root.held) {
            root.wake()
        }
    }
    // Hidden no sooner than the delay after the drag ends.
    onDraggingChanged: {
        if (!root.dragging) {
            root.wake()
        }
    }
    onActiveChanged: {
        if (!root.active) {
            root.dragging = false
        }
        reset()
    }
    onHideDelayChanged: reset()
    Component.onCompleted: reset()

    property QtObject state: QtObject {
        property bool controlsVisible: true
        property bool hasPointerPosition: false
        property real pointerX: 0
        property real pointerY: 0
    }

    property Timer hideTimer: Timer {
        interval: root.hideDelay
        repeat: false
        onTriggered: {
            if (root.active) {
                state.controlsVisible = false
            }
        }
    }
}
