// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// The visualization's mode switch: two tool buttons on one plate,
// Spectrogram and Waveform, the current one checked, as a neutral plate
// (PlotToolButton's `segment`). Set `revealed` from the pane's pointer and
// `focusInside`: hidden, the buttons stay Tab stops. The buttons are 4 px
// apart with 3 px around them, so a button's focus ring, 3 px out from it,
// stays inside the plate and 1 px clear of the other button, whose checked
// edge it never touches (#373 H14 G4: 2 px apart and in, the ring ran over
// the plate's edge and onto the chosen one's).
Components.PlotPlate {
    id: root
    objectName: "visualizationModeSwitch"
    padding: 3
    contentSpacing: 4

    required property int selectedMode
    required property var setMode
    property var pointerActivity: null
    readonly property bool pointerHovered: switchHover.hovered
    // Whether keyboard focus is on either button.
    readonly property bool focusInside: {
        for (let i = 0; i < buttons.count; ++i) {
            const button = buttons.itemAt(i)
            if (button && button.activeFocus) {
                return true
            }
        }
        return false
    }

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
        id: buttons
        // The spectrogram's glyph is the app icon's glide and harmonic,
        // which says frequency over time (#373 H14 G7: Breeze's bar chart
        // said statistics); the waveform's is Breeze's.
        model: [
            { name: I18n.i18nc("@option:radio visualization mode", "Spectrogram"),
              iconName: "", iconSource: Qt.resolvedUrl("../assets/spectrogram-symbolic.svg") },
            { name: I18n.i18nc("@option:radio visualization mode", "Waveform"),
              iconName: "waveform", iconSource: "" }
        ]

        delegate: Components.PlotToolButton {
            required property int index
            required property var modelData
            objectName: "visualizationModeButton" + index
            plotColors: root.plotColors
            uiPalette: root.uiPalette
            text: modelData.name
            icon.name: modelData.iconName
            icon.source: modelData.iconSource
            segment: true
            checked: root.selectedMode === index
            Accessible.onPressAction: root.setMode(index)
            onClicked: root.setMode(index)
        }
    }
}
