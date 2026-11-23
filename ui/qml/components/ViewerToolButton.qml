// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/KeyUtils.js" as KeyUtils

// A flat 24 px tool button, larger with a larger font, on the window's
// chrome or view colour, such as the viewers' info and close buttons: a
// theme icon (`icon.name`), a symbolic mask of the app's own
// (`icon.source`), or its text with `display:
// AbstractButton.TextOnly`. Its `text` is the tooltip and the accessible
// name; `toolTipText` replaces the tooltip. Checked is the selection tint
// alone on the window's colours; the 2 px ring is for keyboard focus only.
// With `plotColors` (UiPalette's PlotColors), it takes its colours from the
// plot it sits on rather than the window, as on a viewer's title strip
// over the view, and checked is the mode switch's chosen segment: the
// plot's text colour at 18 % inside a 1 px edge of its segmentEdge, so the
// plates have one way to show a choice that is on (not an accent tint).
// A checked toggle, which a click turns off, answers the pointer: its fill
// goes to 26 % while hovered (at one fill it read as inert, though a click
// changed it); a chosen segment of a choice, which a click leaves chosen,
// stays as it is.
ToolButton {
    id: root

    required property var uiPalette
    property var plotColors: null
    property string toolTipText: root.text
    readonly property color baseTextColor: root.plotColors
        ? root.plotColors.plotText : root.uiPalette.text
    property color glyphColor: root.baseTextColor
    // On a plot, hover is a wash of its text colour that reads on the
    // strip and the plates: 14 % on a dark one, some 1.4:1, and 12 % on a
    // light one, where the same tint steps more (at 6.5 %, 1.15:1 on a
    // 24 px target, it hardly showed); pressed is 22 %. On the window's
    // colours, its hover tint.
    readonly property bool _plotIsDark: root.plotColors !== null
        && ColorUtils.colorLuma(root.plotColors.plotBg) < 0.45
    readonly property color hoverColor: root.plotColors
        ? ColorUtils.withAlpha(root.baseTextColor, root._plotIsDark ? 0.14 : 0.12)
        : root.uiPalette.hover
    readonly property color pressedColor: ColorUtils.withAlpha(root.baseTextColor,
        root.plotColors ? 0.22 : 0.10)
    // Whether a click turns it off: checked, and not one of a choice.
    readonly property bool _togglesOff: root.checked && !root.autoExclusive
    // On a plot, how much of its text colour the checked fill is: 18 %, and
    // 26 % while the pointer is on a toggle that a click turns off. Only the
    // amount fades, so a change of plot recolours it at once.
    property real _checkedAmount: root._togglesOff && root.hovered ? 0.26 : 0.18
    Behavior on _checkedAmount {
        enabled: Kirigami.Units.shortDuration > 0
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
        }
    }
    readonly property color checkedColor: root.plotColors
        ? ColorUtils.withAlpha(root.plotColors.plotText, root._checkedAmount) : root.uiPalette.selection
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
    // A press takes the focus too, for the mouse: a button the keyboard
    // focused before lets it go, and with it its ring (with Tab focus
    // alone, Close kept its ring while the pointer pressed Info).
    focusPolicy: Qt.StrongFocus

    Accessible.name: root.text

    // Space presses the button the keyboard focused rather than playing or
    // pausing; one a click focused leaves it to the shortcut, as it did
    // before a click took the focus. Keys with modifiers stay the
    // shortcuts'.
    Keys.onShortcutOverride: function(event) {
        event.accepted = root.focusReason !== Qt.MouseFocusReason && KeyUtils.isPlainSpace(event)
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
            // A theme icon by name, or the app's own glyph by its URL.
            source: root.icon.name.length > 0 ? root.icon.name : root.icon.source
            color: root.glyphColor
            isMask: true
        }

        Label {
            id: label
            objectName: "viewerToolButtonLabel"
            anchors.centerIn: parent
            visible: root.display === AbstractButton.TextOnly
            textFormat: Text.PlainText
            text: root.text
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.uiPalette.smallPointSize
            color: root.glyphColor
            Accessible.ignored: true
        }
    }

    background: Item {
        // Only the hover fades. It steps aside while the button is down,
        // so the pressed fill below shows alone and whole from the first
        // frame, from the keyboard as from the pointer.
        Rectangle {
            objectName: "viewerToolButtonHover"
            anchors.fill: parent
            radius: 3
            color: root.hoverColor
            opacity: root.hovered && !root.checked ? 1 : 0
            visible: opacity > 0 && !root.down

            Behavior on opacity {
                enabled: Kirigami.Units.shortDuration > 0
                NumberAnimation {
                    duration: Kirigami.Units.shortDuration
                    easing.type: Easing.OutCubic
                }
            }
        }

        Rectangle {
            objectName: "viewerToolButtonPressed"
            anchors.fill: parent
            radius: 3
            visible: root.down && !root.checked
            color: root.pressedColor
        }

        // Checked: the fill, and on a plot its edge; never the focus ring.
        Rectangle {
            objectName: "viewerToolButtonChecked"
            anchors.fill: parent
            radius: 3
            visible: root.checked
            color: root.checkedColor
        }

        Rectangle {
            objectName: "viewerToolButtonCheckedEdge"
            anchors.fill: parent
            radius: 3
            visible: root.checked && root.plotColors !== null
            color: "transparent"
            border.width: 1
            border.color: root.plotColors ? root.plotColors.segmentEdge : "transparent"
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
