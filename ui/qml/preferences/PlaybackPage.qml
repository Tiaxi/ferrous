// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/FormatUtils.js" as FormatUtils
import "." as Preferences

// The Playback page: ReplayGain, which plays tracks at a similar loudness
// from the gain tags in their files. Its other settings mean nothing while
// it is off, so they are disabled then, as General's title strip is for a
// viewer in the window.
Preferences.PreferencesPage {
    id: root

    title: I18n.i18nc("@title preferences page", "Playback")

    readonly property int replayGainMode: Math.max(0, Math.min(2,
        Number(root.settings.value("replayGainMode")) || 0))
    readonly property bool replayGainOn: root.replayGainMode > 0

    // A gain in dB, -15 to +15 in 0.5 dB steps, which it holds in half
    // decibels: "−6.5 dB". It takes the text typed with either separator
    // and with or without its unit.
    component GainSpinBox: SpinBox {
        id: spinBox

        required property var formSettings
        required property string settingKey

        from: -30
        to: 30
        stepSize: 1
        editable: true
        Layout.preferredWidth: Math.max(Kirigami.Units.gridUnit * 7, implicitWidth)
        value: Math.round((Number(spinBox.formSettings.value(spinBox.settingKey)) || 0) * 2)
        onValueModified: spinBox.formSettings.setValue(spinBox.settingKey, spinBox.value / 2)
        textFromValue: function(value) {
            return FormatUtils.formatGainDb(value / 2)
        }
        valueFromText: function(text) {
            const number = String(text).replace("−", "-").replace(",", ".")
                .replace(/[^-+0-9.]/g, "")
            const db = parseFloat(number)
            return isFinite(db) ? Math.round(db * 2) : spinBox.value
        }
        validator: RegularExpressionValidator {
            regularExpression: /\s*[-+−]?\d{0,2}([.,]\d?)?\s*(dB)?\s*/
        }
    }

    Components.DialogComboBox {
        id: modeComboBox
        objectName: "replayGainModeComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "ReplayGain:")
        Layout.preferredWidth: Math.max(Kirigami.Units.gridUnit * 11, implicitWidth)
        model: [
            I18n.i18nc("@item:inlistbox ReplayGain mode", "Off"),
            I18n.i18nc("@item:inlistbox ReplayGain mode", "Track gain"),
            I18n.i18nc("@item:inlistbox ReplayGain mode", "Album gain")
        ]
        currentIndex: root.replayGainMode
        onActivated: function(index) { root.settings.setValue("replayGainMode", index) }
    }

    Components.FormHelpText {
        objectName: "replayGainHelp"
        uiPalette: root.uiPalette
        text: I18n.i18nc("@info", "Plays tracks at a similar loudness, using the gain tags in their files. "
            + "Album gain keeps the tracks of an album as loud as each other, as they were mastered. "
            + "Ferrous doesn’t measure files; a tagger such as rsgain adds the tags.")
    }

    GainSpinBox {
        objectName: "replayGainPreampSpinBox"
        formSettings: root.settings
        settingKey: "replayGainPreampDb"
        enabled: root.replayGainOn
        Kirigami.FormData.label: I18n.i18nc("@label:spinbox", "Preamp:")
        Accessible.name: I18n.i18nc("@label:spinbox", "Preamp")
    }

    GainSpinBox {
        objectName: "replayGainUntaggedSpinBox"
        formSettings: root.settings
        settingKey: "replayGainUntaggedDb"
        enabled: root.replayGainOn
        Kirigami.FormData.label: I18n.i18nc("@label:spinbox", "Files without gain tags:")
        Accessible.name: I18n.i18nc("@label:spinbox", "Files without gain tags")
    }

    Components.DialogComboBox {
        objectName: "replayGainShowComboBox"
        enabled: root.replayGainOn
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Show the applied gain:")
        Layout.preferredWidth: Math.max(Kirigami.Units.gridUnit * 11, implicitWidth)
        model: [
            I18n.i18nc("@item:inlistbox where the applied ReplayGain shows", "In the status bar"),
            I18n.i18nc("@item:inlistbox where the applied ReplayGain shows", "Only in the tooltip")
        ]
        currentIndex: root.settings.value("showReplayGainInStatusBar") === false ? 1 : 0
        onActivated: function(index) { root.settings.setValue("showReplayGainInStatusBar", index === 0) }
    }
}
