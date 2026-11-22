// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// A lane's label, and 8 px right of it its Mute and Solo buttons on one
// plate. The buttons show while the pointer is on the lane or the plate, and
// while keyboard focus is on either of them, and in a viewer with its
// controls; hidden, they stay Tab stops, so the keyboard reveals them.
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
    readonly property bool labelHovered: root.pointerKnown
        && root.pointerPosition.x >= root.x && root.pointerPosition.x < root.x + labelPlate.width
        && root.pointerPosition.y >= root.y && root.pointerPosition.y < root.y + labelPlate.height
    // Whether the pointer is on the label's plate or the buttons': the
    // plot's crosshair gives way there (PlotLayer's crosshairLevel).
    readonly property bool plateHovered: root.labelHovered || buttonsHover.hovered
    // Whether the buttons show by their own rule; the viewer's controls
    // hiding fades them through `controlsLevel` instead, on its curve.
    readonly property bool buttonsRevealed: root.buttonsVisibility === 2
        || (root.buttonsVisibility === 1
            && ((root.revealedWithControls && !root.compact) || root.laneHovered
                || buttonsHover.hovered || root.focusInside || root.muted || root.soloed))
    // The buttons' size: the label's height and 2 px above and below it,
    // compact; otherwise the design's small tool button.
    readonly property real buttonSize: root.compact
        ? labelPlate.height + 4
        : root.uiPalette.toolButtonSize
    readonly property real glyphPointSize: root.compact
        ? root.uiPalette.compactPointSize
        : root.uiPalette.smallPointSize

    // The label's box; the buttons' plate, taller, is centred on it, so
    // that the label's letters and M and S stand on one centre line (with
    // the plates' tops on one line, they stood 6 px apart). It reaches
    // `plateReach` above the label, which the plot layer insets the head
    // by, so that the plate's top is on one line with the mode switch's,
    // and its centre line with the label's is the switch's.
    readonly property real plateReach: Math.max(0, -buttons.y)
    implicitWidth: buttons.visible ? buttons.x + buttons.width : labelPlate.width
    implicitHeight: labelPlate.height
    // What the head takes of its lane, from its own top left, with the
    // plates' 1 px edges. The buttons count while they can show, so that it
    // stays the same as they reveal and hide.
    readonly property rect occupiedBounds: {
        const top = (buttons.visible ? Math.min(0, buttons.y) : 0) - 1
        const bottom = (buttons.visible
            ? Math.max(labelPlate.height, buttons.y + buttons.height)
            : labelPlate.height) + 1
        return Qt.rect(-1, top, root.implicitWidth + 2, bottom - top)
    }
    width: implicitWidth
    height: implicitHeight

    Components.PlotPlate {
        id: labelPlate
        objectName: "plotLaneLabel"
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: root.label
        compact: root.compact
        fadeLevel: root.labelLevel

    }

    Components.PlotPlate {
        id: buttons
        objectName: "plotLaneButtons"
        x: labelPlate.width + 8
        y: Math.round((labelPlate.height - buttons.height) / 2)
        // 4 px around the buttons and between them, as on the mode switch,
        // so a button's focus ring, 3 px out from it, keeps 1 px of the
        // plate outside it.
        padding: 4
        contentSpacing: 4
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
