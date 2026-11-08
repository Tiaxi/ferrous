// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// A lane's label, and 8 px right of it its Mute and Solo buttons on one
// plate. The buttons show while the pointer is on the lane or the plate, and
// while keyboard focus is on either of them; hidden, they stay Tab stops,
// so the keyboard reveals them.
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
    property bool laneHovered: false
    property var toggleMute: function() {}
    property var toggleSolo: function() {}
    property var pointerActivity: null
    readonly property bool focusInside: muteButton.activeFocus || soloButton.activeFocus
    readonly property bool buttonsRevealed: root.buttonsVisibility === 2
        || (root.buttonsVisibility === 1
            && (root.laneHovered || buttonsHover.hovered || root.focusInside))

    // The label's box: the buttons' plate, taller, is centred on it and
    // overhangs it above and below.
    implicitWidth: buttons.visible ? buttons.x + buttons.width : labelPlate.width
    implicitHeight: labelPlate.height
    width: implicitWidth
    height: implicitHeight

    Components.PlotPlate {
        id: labelPlate
        objectName: "plotLaneLabel"
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: root.label
    }

    Components.PlotPlate {
        id: buttons
        objectName: "plotLaneButtons"
        x: labelPlate.width + 8
        anchors.verticalCenter: labelPlate.verticalCenter
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        visible: root.buttonsVisibility !== 0
        revealed: root.buttonsRevealed
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
            glyphText: I18n.i18nc("@action:button one-letter abbreviation of mute", "M")
            text: I18n.i18nc("@action:button %1 is a channel", "Mute %1", root.label)
            checked: root.muted
            Accessible.role: Accessible.CheckBox
            Accessible.checkable: true
            Accessible.checked: checked
            Accessible.onPressAction: root.toggleMute()
            onClicked: root.toggleMute()
        }

        Components.PlotToolButton {
            id: soloButton
            objectName: "plotLaneSoloButton"
            plotColors: root.plotColors
            uiPalette: root.uiPalette
            glyphText: I18n.i18nc("@action:button one-letter abbreviation of solo", "S")
            text: I18n.i18nc("@action:button %1 is a channel", "Solo %1", root.label)
            checked: root.soloed
            Accessible.role: Accessible.CheckBox
            Accessible.checkable: true
            Accessible.checked: checked
            Accessible.onPressAction: root.toggleSolo()
            onClicked: root.toggleSolo()
        }
    }
}
