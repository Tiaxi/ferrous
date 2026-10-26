// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Shapes 1.15

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
            readonly property color iconColor: root.selectedMode === index
                ? "#eef1f3" : "#aeb6bc"
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
                            color: button.iconColor
                        }
                    }
                }

                // Drawn from its colour, so it follows the selected mode.
                Shape {
                    anchors.fill: parent
                    visible: button.index === 1
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        objectName: "visualizationModeWaveformStroke"
                        strokeColor: button.iconColor
                        strokeWidth: 1
                        fillColor: "transparent"
                        startX: 0
                        startY: 7
                        PathLine { x: 3; y: 4 }
                        PathLine { x: 6; y: 9 }
                        PathLine { x: 9; y: 2 }
                        PathLine { x: 13; y: 6 }
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
