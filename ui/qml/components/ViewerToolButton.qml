// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/KeyUtils.js" as KeyUtils

// A flat 24 px tool button, larger with a larger font, on the window's
// chrome or view colour, such as the viewers' info and close buttons: a
// theme icon (`icon.name`), or its text with `display:
// AbstractButton.TextOnly`. Its `text` is the tooltip and the accessible
// name; `toolTipText` replaces the tooltip. Checked is
// the selection tint alone; the 2 px ring is for keyboard focus only.
// With `plotColors` (UiPalette's PlotColors), it takes its colours from the
// plot it sits on rather than the window, as on a viewer's title strip
// over the view.
ToolButton {
    id: root

    required property var uiPalette
    property var plotColors: null
    property string toolTipText: root.text
    readonly property color baseTextColor: root.plotColors
        ? root.plotColors.plotText : root.uiPalette.text
    property color glyphColor: root.baseTextColor
    // The window's hover tint, worked out the same way on the plot's floor.
    readonly property bool _plotIsDark: root.plotColors !== null
        && ColorUtils.colorLuma(root.plotColors.plotBg) < 0.45
    readonly property color hoverColor: root.plotColors
        ? ColorUtils.withAlpha(root.baseTextColor, root._plotIsDark ? 0.065 : 0.08)
        : root.uiPalette.hover
    readonly property color checkedColor: root.plotColors
        ? root.plotColors.checkedTint : root.uiPalette.selection
    readonly property color focusRingColor: root.plotColors
        ? root.plotColors.plotFocusRing : root.uiPalette.focusRing

    implicitWidth: root.display === AbstractButton.TextOnly
        ? Math.max(root.uiPalette.toolButtonSize, label.implicitWidth + 2 * 6)
        : root.uiPalette.toolButtonSize
    implicitHeight: root.uiPalette.toolButtonSize
    padding: 0
    flat: true
    hoverEnabled: true
    display: AbstractButton.IconOnly
    icon.width: root.uiPalette.toolIconSize
    icon.height: root.uiPalette.toolIconSize
    focusPolicy: Qt.TabFocus

    Accessible.name: root.text

    // Space presses the focused button rather than playing or pausing; keys
    // with modifiers stay the shortcuts'.
    Keys.onShortcutOverride: function(event) {
        event.accepted = KeyUtils.isPlainSpace(event)
    }

    ToolTip.text: root.toolTipText
    ToolTip.visible: (root.hovered || root.visualFocus) && root.toolTipText.length > 0
    ToolTip.delay: Kirigami.Units.toolTipDelay

    contentItem: Item {
        Kirigami.Icon {
            objectName: "viewerToolButtonIcon"
            anchors.centerIn: parent
            width: root.icon.width
            height: root.icon.height
            visible: root.display !== AbstractButton.TextOnly
            source: root.icon.name
            color: root.glyphColor
            isMask: true
        }

        Label {
            id: label
            objectName: "viewerToolButtonLabel"
            anchors.centerIn: parent
            visible: root.display === AbstractButton.TextOnly
            text: root.text
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.uiPalette.smallPointSize
            color: root.glyphColor
            Accessible.ignored: true
        }
    }

    background: Item {
        Rectangle {
            objectName: "viewerToolButtonHover"
            anchors.fill: parent
            radius: 3
            color: root.down
                ? ColorUtils.withAlpha(root.baseTextColor, 0.10)
                : root.hoverColor
            opacity: (root.hovered || root.down) && !root.checked ? 1 : 0
            visible: opacity > 0

            Behavior on opacity {
                enabled: Kirigami.Units.shortDuration > 0
                NumberAnimation {
                    duration: Kirigami.Units.shortDuration
                    easing.type: Easing.OutCubic
                }
            }
        }

        // Checked: the tint alone, never a ring.
        Rectangle {
            objectName: "viewerToolButtonChecked"
            anchors.fill: parent
            radius: 3
            visible: root.checked
            color: root.checkedColor
        }

        Rectangle {
            objectName: "viewerToolButtonFocusRing"
            anchors.fill: parent
            radius: 3
            visible: root.visualFocus
            color: "transparent"
            border.width: 2
            border.color: root.focusRingColor
        }
    }
}
