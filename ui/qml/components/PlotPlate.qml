// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15

// A plate on a visualization, in the plot's own colours (UiPalette's
// PlotColors) with a 1 px edge outside it. With `text`, it is a lane label;
// otherwise it holds the items declared inside it, such as PlotToolButtons,
// 2 px apart with 2 px around them.
Item {
    id: root

    required property var plotColors
    required property var uiPalette
    property string text: ""
    readonly property bool isLabel: root.text.length > 0
    property real horizontalPadding: root.isLabel ? 6 : 2
    property real verticalPadding: root.isLabel ? 0 : 2
    property real radius: root.isLabel ? 3 : 4
    default property alias contentData: content.data

    implicitWidth: Math.max(20, content.implicitWidth + 2 * root.horizontalPadding)
    implicitHeight: Math.max(20, content.implicitHeight + 2 * root.verticalPadding)

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

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 2

        Label {
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
