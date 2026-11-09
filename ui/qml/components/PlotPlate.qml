// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// A plate on a visualization, in the plot's own colours (UiPalette's
// PlotColors) with a 1 px edge outside it. With `text`, it is a lane label;
// otherwise it holds the items declared inside it, such as PlotToolButtons,
// 2 px apart with 2 px around them. A plate that is not `revealed` fades
// out but keeps its place and its Tab stops, so the keyboard can reveal it.
Item {
    id: root

    required property var plotColors
    required property var uiPalette
    property string text: ""
    readonly property bool isLabel: root.text.length > 0
    property real horizontalPadding: root.isLabel ? 6 : 2
    property real verticalPadding: root.isLabel ? 0 : 2
    property real radius: root.isLabel ? 3 : 4
    // Whether presses, clicks and double-clicks on the plate, between its
    // buttons too, stay on it rather than reach the view under it.
    property bool takesPointer: false
    property bool revealed: true
    default property alias contentData: content.data

    implicitWidth: Math.max(20, content.implicitWidth + 2 * root.horizontalPadding)
    implicitHeight: Math.max(20, content.implicitHeight + 2 * root.verticalPadding)

    opacity: root.revealed ? 1 : 0

    // Kirigami's durations are 0 with animations turned off.
    Behavior on opacity {
        enabled: Kirigami.Units.shortDuration > 0
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
        }
    }

    Accessible.role: root.isLabel ? Accessible.StaticText : Accessible.Grouping
    Accessible.name: root.text

    // Outside the plate: a border drawn inside a rectangle 1 px larger.
    Rectangle {
        objectName: "plotPlateEdge"
        x: -1
        y: -1
        width: parent.width + 2
        height: parent.height + 2
        radius: root.radius + 1
        color: "transparent"
        border.width: 1
        border.color: root.plotColors.plotPlateEdge
    }

    Rectangle {
        objectName: "plotPlateFill"
        anchors.fill: parent
        radius: root.radius
        color: root.plotColors.plotPlate
    }

    MouseArea {
        objectName: "plotPlatePointerArea"
        anchors.fill: parent
        anchors.margins: -1
        enabled: root.takesPointer
        visible: root.takesPointer
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        // Taken, so a double-click on the plate never reaches the view's.
        onDoubleClicked: function(mouse) { mouse.accepted = true }
    }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 2

        // Text, not a Label: the desktop style's Label has a hover handler,
        // which would take the hover from the view under the plate.
        Text {
            objectName: "plotPlateLabel"
            visible: root.isLabel
            text: root.text
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.uiPalette.smallPointSize
            font.weight: Font.DemiBold
            color: root.plotColors.plotText
            height: 20
            verticalAlignment: Text.AlignVCenter
            Accessible.ignored: true
        }
    }
}
