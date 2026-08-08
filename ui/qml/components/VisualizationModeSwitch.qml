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
    spacing: 1
    visible: controlsVisible
    opacity: controlsVisible && proximityHovered ? 1 : 0
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
            width: 24
            height: 22
            radius: 2
            color: root.selectedMode === index
                ? Qt.rgba(0.24, 0.27, 0.31, 0.94)
                : (hovered ? Qt.rgba(0.12, 0.14, 0.16, 0.9)
                           : Qt.rgba(0.03, 0.04, 0.05, 0.78))
            border.color: root.selectedMode === index
                ? Qt.rgba(1, 1, 1, 0.34)
                : Qt.rgba(1, 1, 1, 0.16)

            Accessible.role: Accessible.Button
            Accessible.name: modelData
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
                enabled: root.opacity > 0.5
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
