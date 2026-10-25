// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

Row {
    id: root
    objectName: "visualizationModeSwitch"

    required property int selectedMode
    required property var setMode
    property bool controlsVisible: true
    property var pointerActivity: null
    property bool proximityHovered: false
    // A button of the switch has keyboard focus.
    property bool keyboardFocused: false
    // Stays visible, dimmed until the pointer comes close or a button
    // has keyboard focus.
    readonly property real idleOpacity: 0.55
    spacing: 1
    visible: controlsVisible
    opacity: !controlsVisible ? 0 : (proximityHovered || keyboardFocused ? 1 : idleOpacity)
    onControlsVisibleChanged: {
        if (!controlsVisible) {
            proximityHovered = false
        }
    }

    Behavior on opacity {
        NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
    }

    HoverHandler {
        id: proximityHover
        enabled: root.controlsVisible
        margin: 24
        onHoveredChanged: root.proximityHovered = hovered
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(
                    point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    Repeater {
        model: ["Spectrogram", "Waveform"]

        delegate: Rectangle {
            id: button
            required property int index
            required property string modelData
            property bool hovered: false
            objectName: "visualizationModeButton" + index
            width: 24
            height: 22
            radius: 2
            activeFocusOnTab: true
            color: root.selectedMode === index
                ? Qt.rgba(0.24, 0.27, 0.31, 0.94)
                : (hovered || activeFocus ? Qt.rgba(0.12, 0.14, 0.16, 0.9)
                                          : Qt.rgba(0.03, 0.04, 0.05, 0.78))
            border.width: activeFocus ? 2 : 1
            border.color: activeFocus
                ? Qt.rgba(0.55, 0.75, 1, 0.95)
                : (root.selectedMode === index
                    ? Qt.rgba(1, 1, 1, 0.34)
                    : Qt.rgba(1, 1, 1, 0.16))

            onActiveFocusChanged: root.keyboardFocused = activeFocus

            // Space presses the focused button rather than playing or pausing.
            Keys.onShortcutOverride: function(event) {
                event.accepted = event.key === Qt.Key_Space
            }
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Space
                        || event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    root.setMode(button.index)
                    event.accepted = true
                }
            }

            Accessible.role: Accessible.RadioButton
            Accessible.name: modelData
            Accessible.focusable: true
            Accessible.checkable: true
            Accessible.checked: root.selectedMode === index
            Accessible.onPressAction: root.setMode(index)

            Item {
                anchors.centerIn: parent
                width: 14
                height: 12

                Row {
                    anchors.centerIn: parent
                    spacing: 1
                    visible: button.index === 0

                    Repeater {
                        model: [5, 10, 7, 12, 8]
                        delegate: Rectangle {
                            required property int modelData
                            width: 1
                            height: modelData
                            anchors.verticalCenter: parent.verticalCenter
                            color: root.selectedMode === button.index
                                ? "#eef1f3" : "#aeb6bc"
                        }
                    }
                }

                Canvas {
                    anchors.fill: parent
                    visible: button.index === 1
                    onPaint: {
                        const context = getContext("2d")
                        context.reset()
                        context.strokeStyle = root.selectedMode === button.index
                            ? "#eef1f3" : "#aeb6bc"
                        context.lineWidth = 1
                        context.beginPath()
                        context.moveTo(0, 7)
                        context.lineTo(3, 4)
                        context.lineTo(6, 9)
                        context.lineTo(9, 2)
                        context.lineTo(13, 6)
                        context.stroke()
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.controlsVisible
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onEntered: button.hovered = true
                onExited: button.hovered = false
                onPositionChanged: function(mouse) {
                    if (root.pointerActivity) {
                        root.pointerActivity(mouse.x, mouse.y)
                    }
                }
                onClicked: root.setMode(button.index)
            }
        }
    }
}
