// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// A 24 px tool button on a PlotPlate: a theme icon (`icon.name`), or short
// text such as the M and S of a lane. Its `text` is the tooltip and the
// accessible name. Checked is the plot's checked tint with the checked
// glyph colour; keyboard focus is a separate 2 px accent ring inside it;
// both show together.
ToolButton {
    id: root

    required property var plotColors
    required property var uiPalette
    // The text drawn instead of an icon, such as "M"; `text` stays the name.
    property string glyphText: ""
    readonly property color glyphColor: root.checked
        ? root.plotColors.checkedGlyph
        : root.plotColors.plotText

    implicitWidth: 24
    implicitHeight: 24
    padding: 0
    flat: true
    display: AbstractButton.IconOnly
    icon.width: Kirigami.Units.iconSizes.small
    icon.height: Kirigami.Units.iconSizes.small
    icon.color: root.glyphColor
    focusPolicy: Qt.TabFocus

    Accessible.name: root.text

    ToolTip.text: root.text
    ToolTip.visible: root.hovered && root.text.length > 0
    ToolTip.delay: Kirigami.Units.toolTipDelay

    contentItem: Item {
        Kirigami.Icon {
            objectName: "plotToolButtonIcon"
            anchors.centerIn: parent
            width: root.icon.width
            height: root.icon.height
            visible: root.glyphText.length === 0
            source: root.icon.name
            color: root.glyphColor
            isMask: true
        }

        Label {
            objectName: "plotToolButtonGlyph"
            anchors.centerIn: parent
            visible: root.glyphText.length > 0
            text: root.glyphText
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.uiPalette.smallPointSize
            font.weight: Font.DemiBold
            color: root.glyphColor
            Accessible.ignored: true
        }
    }

    background: Item {
        Rectangle {
            objectName: "plotToolButtonHover"
            anchors.fill: parent
            radius: 3
            color: Qt.rgba(root.plotColors.plotText.r, root.plotColors.plotText.g,
                           root.plotColors.plotText.b, 0.12)
            opacity: root.hovered && !root.checked ? 1 : 0
            visible: opacity > 0

            Behavior on opacity {
                enabled: Kirigami.Units.shortDuration > 0
                NumberAnimation {
                    duration: Kirigami.Units.shortDuration
                    easing.type: Easing.OutCubic
                }
            }
        }

        Rectangle {
            objectName: "plotToolButtonChecked"
            anchors.fill: parent
            radius: 3
            visible: root.checked
            color: root.plotColors.checkedTint
        }

        Rectangle {
            objectName: "plotToolButtonFocusRing"
            anchors.fill: parent
            radius: 3
            visible: root.visualFocus
            color: "transparent"
            border.width: 2
            border.color: root.uiPalette.accent
        }
    }
}
