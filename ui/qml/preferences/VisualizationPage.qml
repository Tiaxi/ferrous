// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Preferences

// The Visualization page, in three groups under their subheadings: what
// the plot analyses, what it shows, and how it answers the pointer. The
// "Show:" check boxes stay together.
Preferences.PreferencesPage {
    id: root

    required property var spectrogramFftChoices

    // A combo box's width in the form, or what its longest option needs.
    readonly property real comboWidth: Kirigami.Units.gridUnit * 9

    title: I18n.i18nc("@title preferences page", "Visualization")

    Components.FormGroupGap {
        objectName: "visualizationAnalysisHeading"
        uiPalette: root.uiPalette
        first: true
        heading: I18n.i18nc("@title:group visualization preferences", "Analysis")
    }

    Components.DialogComboBox {
        objectName: "spectrogramViewComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Channels:")
        Layout.preferredWidth: Math.max(root.comboWidth, implicitWidth)
        model: [
            I18n.i18nc("@item:inlistbox spectrogram view", "Downmix"),
            I18n.i18nc("@item:inlistbox spectrogram view", "Per channel")
        ]
        currentIndex: Math.max(0, Math.min(1, root.settings.value("spectrogramViewMode")))
        onActivated: function(index) { root.settings.setValue("spectrogramViewMode", index) }
    }

    Components.DialogComboBox {
        objectName: "spectrogramFftComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "FFT window:")
        Layout.preferredWidth: Math.max(root.comboWidth, implicitWidth)
        model: root.spectrogramFftChoices
        currentIndex: {
            const index = root.spectrogramFftChoices.indexOf(root.settings.value("fftSize"))
            return index >= 0 ? index : 0
        }
        onActivated: function(index) {
            root.settings.setValue("fftSize", root.spectrogramFftChoices[Math.max(0, index)])
        }
    }

    // The slider takes the column's width up to 18 grid units, and gives
    // way before its value does, which always shows.
    RowLayout {
        id: dbRangeRow
        objectName: "dbRangeRow"
        Kirigami.FormData.label: I18n.i18nc("@label:slider", "Dynamic range:")
        Layout.fillWidth: true
        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
        Layout.minimumWidth: Kirigami.Units.gridUnit * 6 + dbRangeRow.spacing + dbRangeValueLabel.Layout.minimumWidth
        Layout.maximumWidth: Kirigami.Units.gridUnit * 18
        spacing: Kirigami.Units.largeSpacing

        Slider {
            id: dbRangeSlider
            objectName: "dbRangeSlider"
            Layout.fillWidth: true
            Layout.minimumWidth: Kirigami.Units.gridUnit * 6
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
            id: dbRangeValueLabel
            objectName: "dbRangeValueLabel"
            text: I18n.i18nc("@label:slider value in decibels", "%1 dB", Math.round(dbRangeSlider.value))
            font.features: root.uiPalette.tabularFigures
            Layout.minimumWidth: Math.ceil(dbRangeMetrics.advanceWidth)
            horizontalAlignment: Text.AlignRight

            TextMetrics {
                id: dbRangeMetrics
                font: dbRangeValueLabel.font
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
        objectName: "frequencyCapHelp"
        uiPalette: root.uiPalette
        indent: frequencyCapCheckBox.contentItem.x
        // The next group's subheading leaves the room to it.
        trailingSpace: 0
        text: I18n.i18nc("@info", "High-resolution tracks show up to 22.05 kHz for rates of the 44.1 kHz "
            + "family and 24 kHz for others, instead of up to half their sample rate.")
    }

    Components.FormGroupGap {
        objectName: "visualizationDisplayHeading"
        uiPalette: root.uiPalette
        heading: I18n.i18nc("@title:group visualization preferences", "Display")
    }

    Components.DialogComboBox {
        objectName: "spectrogramDisplayComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Scrolling:")
        Layout.preferredWidth: Math.max(root.comboWidth, implicitWidth)
        model: [
            I18n.i18nc("@item:inlistbox spectrogram display", "Rolling"),
            I18n.i18nc("@item:inlistbox spectrogram display", "Centered")
        ]
        currentIndex: Math.max(0, Math.min(1, root.settings.value("spectrogramDisplayMode")))
        onActivated: function(index) { root.settings.setValue("spectrogramDisplayMode", index) }
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
        objectName: "crosshairHelp"
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
        objectName: "showLevelMeterCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Level meter:")
        text: I18n.i18nc("@option:check", "Show a level meter at the end of each lane")
        checked: root.settings.value("showLevelMeter")
        onToggled: root.settings.setValue("showLevelMeter", checked)
    }

    Components.FormGroupGap {
        objectName: "visualizationInteractionHeading"
        uiPalette: root.uiPalette
        heading: I18n.i18nc("@title:group visualization preferences", "Interaction")
    }

    Components.DialogComboBox {
        objectName: "channelButtonsComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Mute and solo buttons:")
        Layout.preferredWidth: Math.max(root.comboWidth, implicitWidth)
        model: [
            I18n.i18nc("@item:inlistbox channel buttons", "Never"),
            I18n.i18nc("@item:inlistbox channel buttons", "On hover"),
            I18n.i18nc("@item:inlistbox channel buttons", "Always")
        ]
        currentIndex: Math.max(0, Math.min(2, root.settings.value("channelButtonsVisibility")))
        onActivated: function(index) { root.settings.setValue("channelButtonsVisibility", index) }
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
        objectName: "zoomHelp"
        uiPalette: root.uiPalette
        indent: zoomCheckBox.contentItem.x
        text: I18n.i18nc("@info", "Scroll to zoom in or out; middle-click to reset.")
    }

    CheckBox {
        objectName: "stretchShortTracksCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Default zoom:")
        text: I18n.i18nc("@option:check", "Stretch tracks shorter than the view to its full width")
        checked: root.settings.value("stretchShortTracks")
        onToggled: root.settings.setValue("stretchShortTracks", checked)
    }
}
