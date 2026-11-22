// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Preferences

// The General page: the fullscreen viewers, and Plasma's media controls.
// Two groups apart by the room between them, as on Last.fm, with "Media
// controls:" naming the second by what it sets (it was "Plasma:", the
// desktop's name): only a page of three or more groups, Visualization,
// heads them (one heading over a single check box that its label already
// named).
Preferences.PreferencesPage {
    id: root

    title: I18n.i18nc("@title preferences page", "General")

    Components.DialogComboBox {
        objectName: "viewerFullscreenComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Fullscreen viewers:")
        Layout.preferredWidth: Math.max(Kirigami.Units.gridUnit * 11, implicitWidth)
        model: [
            I18n.i18nc("@item:inlistbox viewer fullscreen", "Fill the window"),
            I18n.i18nc("@item:inlistbox viewer fullscreen", "Fill the screen")
        ]
        currentIndex: Math.max(0, Math.min(1, root.settings.value("viewerFullscreenMode")))
        onActivated: function(index) { root.settings.setValue("viewerFullscreenMode", index) }
    }

    CheckBox {
        objectName: "showFullscreenTitleStripCheckBox"
        text: I18n.i18nc("@option:check", "Show the title strip when the pointer moves")
        // Only a viewer over the whole screen has the strip; one in the
        // window has its corner buttons.
        enabled: Math.max(0, Math.min(1, root.settings.value("viewerFullscreenMode"))) === 1
        checked: root.settings.value("showFullscreenTitleStrip")
        onToggled: root.settings.setValue("showFullscreenTitleStrip", checked)
    }

    CheckBox {
        objectName: "preventDisplaySleepInFullscreenCheckBox"
        text: I18n.i18nc("@option:check", "Keep the display awake while playing fullscreen")
        checked: root.settings.value("preventDisplaySleepInFullscreen")
        onToggled: root.settings.setValue("preventDisplaySleepInFullscreen", checked)
    }

    Components.FormGroupGap {
        objectName: "desktopIntegrationGap"
        uiPalette: root.uiPalette
    }

    CheckBox {
        id: mediaCheckBox
        objectName: "systemMediaControlsCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Media controls:")
        // The label has said where; a screen reader hears both, the box
        // alone.
        text: I18n.i18nc("@option:check after the label Media controls", "Show Ferrous and answer media keys")
        Accessible.name: I18n.i18nc("@option:check accessible name; it contains the check box's text",
            "Media controls: Show Ferrous and answer media keys")
        checked: root.settings.value("systemMediaControlsEnabled")
        onToggled: root.settings.setValue("systemMediaControlsEnabled", checked)
    }

    Components.FormHelpText {
        objectName: "systemMediaControlsHelp"
        uiPalette: root.uiPalette
        checkBox: mediaCheckBox
        text: I18n.i18nc("@info", "Ferrous appears in Plasma’s media controls and answers the Play/Pause, "
            + "Previous, Next and Stop keys. The volume keys always set the system volume, not "
            + "Ferrous’s.")
    }
}
