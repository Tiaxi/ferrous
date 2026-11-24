// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// A lane's head: one plate, as the mode switch's and as tall, with the
// lane's label in a cell of its own, a 1 px divider, and Mute and Solo (a
// label's chip beside a taller plate of buttons read as two stickers). The
// cell is as wide as the pane's widest label wants (`widestLabel`), so that
// M and S are at one x in every lane (by each lane's label they wandered
// 12 px from L to LFE). The plate shows while the pointer is on the lane or
// the plate, and while keyboard focus is on either button, and in a viewer
// with its controls; hidden, the buttons stay Tab stops, so the keyboard
// reveals them. At rest the label alone shows, on a chip in the cell's
// place, as far in from the plate's left as from its top, so that it sits
// in the lane's corner as far from the top as from the side (centred in
// the plate's height, it sat 6 px lower than in, H26), and nothing moves as
// Mute and Solo show.
// A `compact` head, in a short lane, such as each of 5.1's in the main
// window, has a 16 px label with 11 px text and 20 px buttons,
// larger with a larger font, and its buttons show only on the pointer or
// keyboard focus, not with a viewer's controls: there, every lane's would
// show at once and crowd the lanes. A lane that is muted or soloed keeps
// its buttons shown, compact or not, so the checked one says why the lane
// is silent, or why the others are, and is there to undo it (a muted
// lane and one silenced by another's Solo looked the same).
Item {
    id: root

    required property var plotColors
    required property var uiPalette
    required property string label
    required property int channelIndex
    property bool muted: false
    property bool soloed: false
    // Never (0), on the pointer or keyboard focus (1), or always (2), as
    // Settings › Display › Channel buttons say.
    property int buttonsVisibility: 1
    // Whether a viewer shows its controls: hidden, the buttons fade out with
    // them, whatever `buttonsVisibility` says, and then take no pointer.
    property bool controlsShown: true
    // The viewer's controls as they fade, which the buttons fade with.
    property real controlsLevel: root.controlsShown ? 1 : 0
    // In a viewer, buttons that show on the pointer show with the viewer's
    // controls, which the pointer's activity shows, on their level alone.
    property bool revealedWithControls: false
    // How much of the label shows here: less where the plot layer fades it
    // over to another place.
    property real labelLevel: 1
    property bool compact: false
    // How much larger than the window's its text is: a viewer's over the
    // whole screen, on its strip's type scale; 1 elsewhere.
    property real textScale: 1
    // The height of the lane, which the label keeps within where that is
    // shorter than it, and how far the plate is moved from the label's
    // centre to stay in the plot, in lanes shorter than it.
    property real laneRoom: Infinity
    property real plateShift: 0
    // The width of the widest label of the pane, in the labels' font.
    property real widestLabel: labelMetrics.advanceWidth(root.label)
    property bool laneHovered: false
    property var toggleMute: function() {}
    property var toggleSolo: function() {}
    property var pointerActivity: null
    readonly property bool focusInside: muteButton.activeFocus || soloButton.activeFocus
    // Where the pointer is over the plot, in the head's parent's
    // coordinates, while `pointerKnown`: the label takes no hover of its
    // own, which would take it from the plot under it and its crosshair.
    property point pointerPosition: Qt.point(0, 0)
    property bool pointerKnown: false
    readonly property bool labelHovered: {
        if (!root.pointerKnown)
            return false
        const onPlate = buttons.visible && buttons.revealed
        const plate = onPlate ? Qt.rect(0, buttons.y, buttons.width, buttons.height)
                              : Qt.rect(labelPlate.x, labelPlate.y, labelPlate.width, labelPlate.height)
        const x = root.pointerPosition.x - root.x
        const y = root.pointerPosition.y - root.y
        return x >= plate.x && x < plate.x + plate.width && y >= plate.y && y < plate.y + plate.height
    }
    // Whether the pointer is on the head's plate, or on the label's chip
    // while that is all of it: the plot's crosshair gives way there
    // (PlotLayer's crosshairLevel).
    readonly property bool plateHovered: root.labelHovered || buttonsHover.hovered
    // Whether the buttons show by their own rule; the viewer's controls
    // hiding fades them through `controlsLevel` instead, on its curve.
    readonly property bool buttonsRevealed: root.buttonsVisibility === 2
        || (root.buttonsVisibility === 1
            && ((root.revealedWithControls && !root.compact) || root.laneHovered
                || buttonsHover.hovered || root.focusInside || root.muted || root.soloed))
    // The buttons' size: compact, the small label's line and 5 px, 20 px
    // with the default font; otherwise the design's small tool button.
    readonly property real buttonSize: root.compact
        ? Math.max(16, Math.ceil(labelMetrics.height) + Math.round(root.uiPalette.fontScale)) + 4
        : root.uiPalette.toolButtonSize
    readonly property real glyphPointSize: (root.compact
        ? root.uiPalette.compactPointSize
        : root.uiPalette.smallPointSize) * root.textScale

    // The plate's, around and between its buttons, as on the mode switch,
    // so that a button's focus ring, 3 px out from it, keeps 1 px of the
    // plate outside it and clears the divider.
    readonly property int platePadding: 4
    // The label's chip: the widest label and the room beside it, and at
    // least a button's width, so a letter's chip is never taller than it
    // is wide.
    readonly property real labelWidth: Math.max(root.buttonSize, Math.ceil(root.widestLabel)
        + 2 * root.uiPalette.scaledSize(root.compact ? 4 : 6))
    // The chip, centred in the plate's height, and in from its left as far
    // as the plot layer puts it from the lane's top (`labelX`): as far as
    // from the plate's top where the plate is as far in from both edges.
    readonly property real labelY: Math.round((root.height - labelPlate.height) / 2)
    property real labelX: root.labelY
    // The label's cell, up to the divider: the chip with the room around it.
    readonly property real cellWidth: 2 * root.labelX + root.labelWidth
    readonly property real plateWidth: root.cellWidth + 1 + 3 * root.platePadding + 2 * root.buttonSize
    // The sizes from the font and the pane alone, at once, rather than as
    // the plate's row is laid out, for the plot's overlays to keep clear.
    implicitWidth: buttons.visible ? root.plateWidth : root.cellWidth
    implicitHeight: root.buttonSize + 2 * root.platePadding
    // What the head takes of its lane, from its own top left, with the
    // plates' 1 px edges. The plate counts while it can show, so that it
    // stays the same as it reveals and hides.
    readonly property rect occupiedBounds: buttons.visible
        ? Qt.rect(-1, root.plateShift - 1, root.plateWidth + 2, root.height + 2)
        : Qt.rect(root.labelX - 1, root.labelY - 1, root.labelWidth + 2, labelPlate.height + 2)
    width: implicitWidth
    height: implicitHeight

    FontMetrics {
        id: labelMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.glyphPointSize
        font.weight: Font.DemiBold
    }

    Components.PlotPlate {
        id: labelPlate
        objectName: "plotLaneLabel"
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: root.label
        compact: root.compact
        textScale: root.textScale
        fadeLevel: root.labelLevel
        z: 1
        x: root.labelX
        y: root.labelY
        width: root.labelWidth
        height: Math.min(implicitHeight, root.laneRoom - 2)
        // Its own fill and edge give way to the shared plate's as that
        // comes in, the plate never thinner between them.
        backgroundLevel: buttons.visible ? 1 - buttons.opacity * buttons.opacity : 1
    }

    Components.PlotPlate {
        id: buttons
        objectName: "plotLaneButtons"
        y: root.plateShift
        width: root.plateWidth
        height: root.height
        padding: root.platePadding
        contentSpacing: root.platePadding
        // The label's, whose hover stays the plot's under it: the plate's
        // own handler is the buttons'.
        leadingCell: root.cellWidth
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        visible: root.buttonsVisibility !== 0 && (root.controlsShown || root.controlsLevel > 0)
        revealed: root.buttonsRevealed
        fadeLevel: root.controlsLevel
        Accessible.name: I18n.i18nc("@label %1 is a channel", "Channel %1", root.label)

        HoverHandler {
            id: buttonsHover
            onPointChanged: {
                if (root.pointerActivity) {
                    root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
                }
            }
        }

        Components.PlotToolButton {
            id: muteButton
            plate: buttons
            objectName: "plotLaneMuteButton"
            plotColors: root.plotColors
            uiPalette: root.uiPalette
            implicitWidth: root.buttonSize
            implicitHeight: root.buttonSize
            glyphPointSize: root.glyphPointSize
            glyphText: I18n.i18nc("@action:button one-letter abbreviation of mute", "M")
            text: I18n.i18nc("@action:button %1 is a channel", "Mute %1", root.label)
            // Checkable, so that assistive technology hears it is a toggle
            // and whether it is on (the accessible state's checkable alone,
            // set over a button that is not, was lost).
            // The lane's state is what it shows: a click's own toggle goes
            // back to it, which the click then changes.
            checkable: true
            checked: root.muted
            onToggled: muteButton.checked = Qt.binding(function() { return root.muted })
            Accessible.onPressAction: root.toggleMute()
            onClicked: root.toggleMute()
        }

        Components.PlotToolButton {
            id: soloButton
            plate: buttons
            objectName: "plotLaneSoloButton"
            plotColors: root.plotColors
            uiPalette: root.uiPalette
            implicitWidth: root.buttonSize
            implicitHeight: root.buttonSize
            glyphPointSize: root.glyphPointSize
            glyphText: I18n.i18nc("@action:button one-letter abbreviation of solo", "S")
            text: I18n.i18nc("@action:button %1 is a channel", "Solo %1", root.label)
            // Checkable, so that assistive technology hears it is a toggle
            // and whether it is on (the accessible state's checkable alone,
            // set over a button that is not, was lost).
            // The lane's state is what it shows: a click's own toggle goes
            // back to it, which the click then changes.
            checkable: true
            checked: root.soloed
            onToggled: soloButton.checked = Qt.binding(function() { return root.soloed })
            Accessible.onPressAction: root.toggleSolo()
            onClicked: root.toggleSolo()
        }
    }
}
