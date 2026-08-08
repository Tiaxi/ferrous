// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

Row {
    id: root
    objectName: "visualizationModeSwitch"

    required property int selectedMode
    required property var setMode
    property bool controlsVisible: true
    spacing: 1
    visible: controlsVisible

    Repeater {
        model: ["Spectrogram", "Waveform"]

        delegate: Rectangle {
            required property int index
            required property string modelData
            width: label.implicitWidth + 16
            height: 26
            radius: 3
            color: root.selectedMode === index
                ? Qt.rgba(0.24, 0.27, 0.31, 0.94)
                : Qt.rgba(0.03, 0.04, 0.05, 0.82)
            border.color: root.selectedMode === index
                ? Qt.rgba(1, 1, 1, 0.32)
                : Qt.rgba(1, 1, 1, 0.16)

            Text {
                id: label
                anchors.centerIn: parent
                text: modelData
                color: root.selectedMode === index ? "#f1f3f5" : "#b8bec5"
                font.pixelSize: 11
                font.weight: root.selectedMode === index ? Font.DemiBold : Font.Normal
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.setMode(index)
            }
        }
    }
}
