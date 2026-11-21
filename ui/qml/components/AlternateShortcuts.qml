// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQml.Models 2.15

// The bindings after the first of the window's ShortcutActions, which take
// only their first themselves: Open's Open key beside Ctrl+O, an alternate
// set in KDE's Shortcuts settings, and a second shortcut the user gave an
// action.
Item {
    id: root

    property var actions: []

    Instantiator {
        model: root.actions

        delegate: Shortcut {
            required property var modelData
            sequences: modelData.keyBindings.slice(1)
            context: Qt.WindowShortcut
            enabled: modelData.enabled
            onActivated: modelData.trigger()
        }
    }
}
