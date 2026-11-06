// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Preferences

// The Media Controls page.
Preferences.PreferencesPage {
    id: root

    title: I18n.i18nc("@title preferences page", "Media Controls")

    CheckBox {
        id: mediaCheckBox
        objectName: "systemMediaControlsCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Plasma:")
        text: I18n.i18nc("@option:check", "Show in the media controls and answer media keys")
        checked: root.settings.value("systemMediaControlsEnabled")
        onToggled: root.settings.setValue("systemMediaControlsEnabled", checked)
    }

    Components.FormHelpText {
        uiPalette: root.uiPalette
        indent: mediaCheckBox.contentItem.x
        text: I18n.i18nc("@info", "Ferrous appears in Plasma’s media controls and answers the Play/Pause, "
            + "Previous, Next and Stop keys. The volume keys always set the system volume, not "
            + "Ferrous’s.")
    }
}
