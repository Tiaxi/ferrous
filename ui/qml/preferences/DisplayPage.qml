// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Preferences

// The Appearance page: the viewers and the level meter.
Preferences.PreferencesPage {
    id: root

    title: I18n.i18nc("@title preferences page", "Appearance")

    Components.DialogComboBox {
        objectName: "viewerFullscreenComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Fullscreen viewers:")
        Layout.preferredWidth: Kirigami.Units.gridUnit * 11
        model: [
            I18n.i18nc("@item:inlistbox viewer fullscreen", "Fill the window"),
            I18n.i18nc("@item:inlistbox viewer fullscreen", "Fill the screen")
        ]
        currentIndex: Math.max(0, Math.min(1, root.settings.value("viewerFullscreenMode")))
        onActivated: function(index) { root.settings.setValue("viewerFullscreenMode", index) }
    }

    CheckBox {
        objectName: "preventDisplaySleepInFullscreenCheckBox"
        text: I18n.i18nc("@option:check", "Keep the display awake while playing fullscreen")
        checked: root.settings.value("preventDisplaySleepInFullscreen")
        onToggled: root.settings.setValue("preventDisplaySleepInFullscreen", checked)
    }

    CheckBox {
        objectName: "showLevelMeterCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Level meter:")
        text: I18n.i18nc("@option:check", "Show a level meter at the end of each lane")
        checked: root.settings.value("showLevelMeter")
        onToggled: root.settings.setValue("showLevelMeter", checked)
    }
}
