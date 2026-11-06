// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// The visualization's mode switch: two tool buttons on one plate,
// Spectrogram and Waveform, the current one checked.
Components.PlotPlate {
    id: root
    objectName: "visualizationModeSwitch"

    required property int selectedMode
    required property var setMode
    property var pointerActivity: null
    readonly property bool pointerHovered: switchHover.hovered

    Accessible.name: I18n.i18nc("@label", "Visualization")

    HoverHandler {
        id: switchHover
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    Repeater {
        model: [
            { name: I18n.i18nc("@option:radio visualization mode", "Spectrogram"),
              icon: "view-statistics" },
            { name: I18n.i18nc("@option:radio visualization mode", "Waveform"),
              icon: "waveform" }
        ]

        delegate: Components.PlotToolButton {
            required property int index
            required property var modelData
            objectName: "visualizationModeButton" + index
            plotColors: root.plotColors
            uiPalette: root.uiPalette
            text: modelData.name
            icon.name: modelData.icon
            checked: root.selectedMode === index
            Accessible.role: Accessible.RadioButton
            Accessible.checkable: true
            Accessible.checked: checked
            Accessible.onPressAction: root.setMode(index)
            onClicked: root.setMode(index)
        }
    }
}
