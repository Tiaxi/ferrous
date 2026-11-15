// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/KeyUtils.js" as KeyUtils

// A 24 px tool button on a PlotPlate, larger with a larger font: a theme
// icon (`icon.name`) or a symbolic mask of the app's own (`icon.source`),
// or short text such as the M and S of a lane. Its `text` is the tooltip and the
// accessible name. Checked is a solid fill with the glyph in the plot's
// checked glyph colour: a state that is on, as a lane's Mute.
//
// A `segment`, one of a plate's choices of which one is always checked,
// as the visualization's mode switch, shows the chosen one as a neutral
// plate instead (#373 H11: the accent's solid blue read as keyboard focus,
// and as the loudest thing on the plot): the plot's text colour at 18 %
// over the plate inside a 1 px edge of the plot's segmentEdge, at least
// 3:1 on the plate (#373 H14 F4), with its glyph in the text colour whole; the others'
// glyphs are at 72 %, whole only while pressed. Under the pointer one of
// the others has a wash of the text colour at 14 % and its glyph at 85 %,
// which show (#373 H14 G4: 8 % and 60 % were 1.2:1, nearly invisible),
// under the chosen one's plate and its edge, so a hovered choice never
// reads as the chosen one (#373 H12 Z4: the 12 % hover with a whole glyph
// came to the plate's brightness). The accent is left to the focus ring. A segment is a
// checkable, auto-exclusive button, so it is a radio button to assistive
// technology, checkable and checked as chosen (#373 H12: as a plain button
// it told of its checked state changing but not that it could be checked),
// and choosing the chosen one again leaves it chosen.
//
// Keyboard focus is a separate 2 px ring 1 px outside the button, with a
// 1 px gap in the plate's colour, so it reads on the plate and around a
// checked fill alike; both show together.
ToolButton {
    id: root

    required property var plotColors
    required property var uiPalette
    // The text drawn instead of an icon, such as "M"; `text` stays the name.
    property string glyphText: ""
    // Its glyph text's size; a short lane's buttons take the compact one.
    property real glyphPointSize: root.uiPalette.smallPointSize
    // One of a plate's choices rather than a state that is on or off.
    property bool segment: false
    readonly property real segmentFillOpacity: 0.18
    readonly property real segmentRestOpacity: 0.72
    readonly property real segmentHoverGlyphOpacity: 0.85
    // The pointer's wash: a segment's, under its chosen plate, and a
    // button's, which checked turns into a solid fill.
    readonly property real segmentHoverOpacity: 0.14
    readonly property real hoverOpacity: root.segment ? root.segmentHoverOpacity : 0.12
    readonly property color hoverColor: Qt.rgba(root.plotColors.plotText.r, root.plotColors.plotText.g,
                                                root.plotColors.plotText.b, root.hoverOpacity)
    readonly property color glyphColor: root.segment
        ? root.plotColors.plotText
        : (root.checked ? root.plotColors.checkedGlyph : root.plotColors.plotText)
    // A segment that is not the chosen one is quieter, with the keyboard's
    // focus too, which the focus ring shows, and a little less so under
    // the pointer; whole only while pressed.
    readonly property real glyphOpacity: !root.segment || root.checked || root.down ? 1
        : (root.hovered ? root.segmentHoverGlyphOpacity : root.segmentRestOpacity)
    readonly property color checkedColor: root.segment
        ? Qt.rgba(root.plotColors.plotText.r, root.plotColors.plotText.g,
                  root.plotColors.plotText.b, root.segmentFillOpacity)
        : root.plotColors.checkedFill

    implicitWidth: root.uiPalette.toolButtonSize
    implicitHeight: root.uiPalette.toolButtonSize
    padding: 0
    flat: true
    // The hover tint, whatever the platform's hover effects.
    hoverEnabled: true
    display: AbstractButton.IconOnly
    icon.width: root.uiPalette.toolIconSize
    icon.height: root.uiPalette.toolIconSize
    icon.color: root.glyphColor
    focusPolicy: Qt.TabFocus
    // The focus ring reaches into the plate's gap to the next button.
    z: root.visualFocus ? 1 : 0

    // A segment checks as a radio button does; its owner binds `checked`
    // to what is chosen and chooses on a click.
    checkable: root.segment
    autoExclusive: root.segment
    Accessible.name: root.text
    // Qt's own roles otherwise: a toggle is a check box.
    Accessible.role: root.segment ? Accessible.RadioButton
        : (root.checkable ? Accessible.CheckBox : Accessible.Button)
    Accessible.checkable: root.checkable
    Accessible.checked: root.checked

    // Space presses the focused button rather than playing or pausing; keys
    // with modifiers stay the shortcuts'.
    Keys.onShortcutOverride: function(event) {
        event.accepted = KeyUtils.isPlainSpace(event)
    }

    ToolTip.text: root.text
    ToolTip.visible: (root.hovered || root.visualFocus) && root.text.length > 0
    ToolTip.delay: Kirigami.Units.toolTipDelay

    contentItem: Item {
        Kirigami.Icon {
            objectName: "plotToolButtonIcon"
            anchors.centerIn: parent
            width: root.icon.width
            height: root.icon.height
            visible: root.glyphText.length === 0
            source: root.icon.name.length > 0 ? root.icon.name : root.icon.source
            color: root.glyphColor
            opacity: root.glyphOpacity
            isMask: true
        }

        // Drawn as curves, with grey antialiasing, as the plate's label is
        // (PlotPlate).
        Label {
            objectName: "plotToolButtonGlyph"
            anchors.centerIn: parent
            visible: root.glyphText.length > 0
            renderType: Text.CurveRendering
            text: root.glyphText
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.glyphPointSize
            font.weight: Font.DemiBold
            color: root.glyphColor
            opacity: root.glyphOpacity
            Accessible.ignored: true
        }
    }

    background: Item {
        Rectangle {
            objectName: "plotToolButtonHover"
            anchors.fill: parent
            radius: 3
            color: root.hoverColor
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
            color: root.checkedColor
        }

        // A chosen segment's 1 px edge, inside it, over its neutral fill:
        // the state at 3:1 or more, which the fill alone is not (#373 H14
        // F4). The focus ring stays outside, 2 px and in the accent.
        Rectangle {
            objectName: "plotToolButtonCheckedEdge"
            anchors.fill: parent
            radius: 3
            visible: root.checked && root.segment
            color: "transparent"
            border.width: 1
            border.color: root.plotColors.segmentEdge
        }

        // The gap between the button and its focus ring: the plate, opaque.
        Rectangle {
            objectName: "plotToolButtonFocusGap"
            anchors.fill: parent
            anchors.margins: -1
            radius: 4
            visible: root.visualFocus
            color: "transparent"
            border.width: 1
            border.color: Qt.tint(root.plotColors.plotBg, root.plotColors.plotPlate)
        }

        Rectangle {
            objectName: "plotToolButtonFocusRing"
            anchors.fill: parent
            anchors.margins: -3
            radius: 6
            visible: root.visualFocus
            color: "transparent"
            border.width: 2
            border.color: root.plotColors.plotFocusRing
        }
    }
}
