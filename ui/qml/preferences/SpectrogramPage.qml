// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Preferences

Preferences.PreferencesPage {
    id: root

    required property var spectrogramFftChoices

    // A control's width in the form.
    readonly property real comboWidth: Kirigami.Units.gridUnit * 9

    title: I18n.i18nc("@title preferences page", "Spectrogram")

    ComboBox {
        objectName: "spectrogramViewComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Channels:")
        Layout.preferredWidth: root.comboWidth
        model: [
            I18n.i18nc("@item:inlistbox spectrogram view", "Downmix"),
            I18n.i18nc("@item:inlistbox spectrogram view", "Per channel")
        ]
        currentIndex: Math.max(0, Math.min(1, root.settings.value("spectrogramViewMode")))
        onActivated: function(index) { root.settings.setValue("spectrogramViewMode", index) }
    }

    ComboBox {
        objectName: "spectrogramDisplayComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Scrolling:")
        Layout.preferredWidth: root.comboWidth
        model: [
            I18n.i18nc("@item:inlistbox spectrogram display", "Rolling"),
            I18n.i18nc("@item:inlistbox spectrogram display", "Centered")
        ]
        currentIndex: Math.max(0, Math.min(1, root.settings.value("spectrogramDisplayMode")))
        onActivated: function(index) { root.settings.setValue("spectrogramDisplayMode", index) }
    }

    ComboBox {
        objectName: "spectrogramFftComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "FFT window:")
        Layout.preferredWidth: root.comboWidth
        model: root.spectrogramFftChoices
        currentIndex: {
            const index = root.spectrogramFftChoices.indexOf(root.settings.value("fftSize"))
            return index >= 0 ? index : 0
        }
        onActivated: function(index) {
            root.settings.setValue("fftSize", root.spectrogramFftChoices[Math.max(0, index)])
        }
    }

    RowLayout {
        Kirigami.FormData.label: I18n.i18nc("@label:slider", "Dynamic range:")
        Layout.preferredWidth: Kirigami.Units.gridUnit * 18
        spacing: Kirigami.Units.largeSpacing

        Slider {
            id: dbRangeSlider
            objectName: "dbRangeSlider"
            Layout.fillWidth: true
            from: 50
            to: 150
            stepSize: 1
            // One tick mark every 10 dB, not one a step.
            Kirigami.StyleHints.tickMarkStepSize: 10
            value: root.settings.value("dbRange")
            onMoved: root.settings.setValue("dbRange", Math.round(value))
            Accessible.name: I18n.i18nc("@label:slider", "Dynamic range")
        }

        Label {
            objectName: "dbRangeValueLabel"
            text: I18n.i18nc("@label:slider value in decibels", "%1 dB", Math.round(dbRangeSlider.value))
            font.features: root.uiPalette.tabularFigures
            Layout.minimumWidth: dbRangeMetrics.advanceWidth
            horizontalAlignment: Text.AlignRight

            TextMetrics {
                id: dbRangeMetrics
                text: I18n.i18nc("@label:slider value in decibels", "%1 dB", 150)
            }
        }
    }

    CheckBox {
        objectName: "spectrogramLogScaleCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Frequency axis:")
        text: I18n.i18nc("@option:check", "Logarithmic scale")
        checked: root.settings.value("logScale")
        onToggled: root.settings.setValue("logScale", checked)
    }

    CheckBox {
        id: frequencyCapCheckBox
        objectName: "spectrogramFrequencyCapCheckBox"
        text: I18n.i18nc("@option:check", "Limit to 22.05 or 24 kHz")
        checked: root.settings.value("spectrogramFrequencyCap")
        onToggled: root.settings.setValue("spectrogramFrequencyCap", checked)
    }

    Components.FormHelpText {
        uiPalette: root.uiPalette
        indent: frequencyCapCheckBox.contentItem.x
        text: I18n.i18nc("@info", "High-resolution tracks show up to 22.05 kHz for rates of the 44.1 kHz "
            + "family and 24 kHz for others, instead of up to half their sample rate.")
    }

    CheckBox {
        objectName: "spectrogramScaleCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Show:")
        text: I18n.i18nc("@option:check", "Frequency and time scale")
        checked: root.settings.value("showSpectrogramScale")
        onToggled: root.settings.setValue("showSpectrogramScale", checked)
    }

    CheckBox {
        id: crosshairCheckBox
        objectName: "spectrogramCrosshairCheckBox"
        text: I18n.i18nc("@option:check", "Crosshair on hover")
        checked: root.settings.value("showSpectrogramCrosshair")
        onToggled: root.settings.setValue("showSpectrogramCrosshair", checked)
    }

    Components.FormHelpText {
        uiPalette: root.uiPalette
        indent: crosshairCheckBox.contentItem.x
        text: I18n.i18nc("@info", "With the crosshair, right-clicking the spectrogram in centered mode "
            + "seeks to that position in the track.")
    }

    CheckBox {
        objectName: "spectrogramFpsCheckBox"
        text: I18n.i18nc("@option:check", "Frame rate")
        checked: root.settings.value("showFps")
        onToggled: root.settings.setValue("showFps", checked)
    }

    CheckBox {
        id: zoomCheckBox
        objectName: "spectrogramZoomCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Zoom:")
        text: I18n.i18nc("@option:check", "Allow zooming")
        checked: root.settings.value("spectrogramZoomEnabled")
        onToggled: root.settings.setValue("spectrogramZoomEnabled", checked)
    }

    Components.FormHelpText {
        uiPalette: root.uiPalette
        indent: zoomCheckBox.contentItem.x
        text: I18n.i18nc("@info", "Scroll to zoom in or out; middle-click to reset.")
    }

    ComboBox {
        objectName: "channelButtonsComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Mute and solo buttons:")
        Layout.preferredWidth: root.comboWidth
        model: [
            I18n.i18nc("@item:inlistbox channel buttons", "Never"),
            I18n.i18nc("@item:inlistbox channel buttons", "On hover"),
            I18n.i18nc("@item:inlistbox channel buttons", "Always")
        ]
        currentIndex: Math.max(0, Math.min(2, root.settings.value("channelButtonsVisibility")))
        onActivated: function(index) { root.settings.setValue("channelButtonsVisibility", index) }
    }
}
