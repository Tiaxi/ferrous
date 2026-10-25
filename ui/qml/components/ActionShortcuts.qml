// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQml.Models 2.15

// The shortcuts of the main window's actions, for a modal popup, which
// blocks the main window's own shortcuts while it is open. Windows whose
// transient parent is the main window already get its shortcuts.
Item {
    id: root

    property var actions: []
    property bool active: false

    Instantiator {
        model: root.actions

        delegate: Shortcut {
            required property var modelData
            sequence: modelData.shortcut
            context: Qt.WindowShortcut
            enabled: root.active && modelData.enabled
            onActivated: modelData.trigger()
        }
    }
}
