// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/KeyUtils.js" as KeyUtils

// A flat 24 px tool button on the window's chrome or view colour, such as
// the viewers' info and close buttons: a theme icon (`icon.name`), or its
// text with `display: AbstractButton.TextOnly`. Its `text` is the tooltip
// and the accessible name; `toolTipText` replaces the tooltip. Checked is
// the selection tint alone; the 2 px ring is for keyboard focus only.
ToolButton {
    id: root

    required property var uiPalette
    property string toolTipText: root.text
    property color glyphColor: root.uiPalette.text

    implicitWidth: root.display === AbstractButton.TextOnly
        ? Math.max(24, label.implicitWidth + 2 * 6)
        : 24
    implicitHeight: 24
    padding: 0
    flat: true
    hoverEnabled: true
    display: AbstractButton.IconOnly
    icon.width: Kirigami.Units.iconSizes.small
    icon.height: Kirigami.Units.iconSizes.small
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
                ? ColorUtils.withAlpha(root.uiPalette.text, 0.10)
                : root.uiPalette.hover
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
            color: root.uiPalette.selection
        }

        Rectangle {
            objectName: "viewerToolButtonFocusRing"
            anchors.fill: parent
            radius: 3
            visible: root.visualFocus
            color: "transparent"
            border.width: 2
            border.color: root.uiPalette.focusRing
        }
    }
}
