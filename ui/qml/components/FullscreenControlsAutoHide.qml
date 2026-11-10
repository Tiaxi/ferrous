// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami

QtObject {
    id: root

    property bool active: false
    property int hideDelay: 5000
    // Keeps the controls shown, such as while one has keyboard focus.
    property bool held: false
    readonly property bool controlsVisible: !root.active || root.held || state.controlsVisible
    // The controls as they fade, 1 shown and 0 hidden, over shortDuration on
    // OutCubic: every overlay that hides with them, the title strip, the
    // corner controls, the mode switch, the lanes' Mute and Solo and the
    // crosshair, takes its opacity from this one level, so none of them
    // cuts while another fades. They take no pointer only once it is 0, and
    // the pointer hides then too.
    property real level: root.controlsVisible ? 1 : 0
    // Kirigami's durations are 0 with animations turned off.
    Behavior on level {
        enabled: Kirigami.Units.shortDuration > 0
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
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
    onActiveChanged: reset()
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
