// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

QtObject {
    id: root

    property bool active: false
    property int hideDelay: 5000
    readonly property bool controlsVisible: !root.active || state.controlsVisible

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

    function reset() {
        hideTimer.stop()
        state.controlsVisible = true
        state.hasPointerPosition = false
        if (root.active) {
            hideTimer.start()
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
