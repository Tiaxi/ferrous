// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls

// A menu's item of an action that stays on where the menu's item cannot
// act, as Stop's, which the transport and the keyboard keep: off, the item
// alone, while `canAct` is false, and while the action is.
//
// Not a binding on `enabled`: the button takes the action's as it takes the
// action and at each change of it, after the bindings that read it have
// run, so a binding was written over. The item follows the action after the
// button has.
MenuItem {
    id: root

    property bool canAct: true

    function followStates() {
        root.enabled = root.canAct && !!root.action && root.action.enabled
    }

    onCanActChanged: root.followStates()
    Component.onCompleted: {
        root.followStates()
        if (root.action) {
            root.action.enabledChanged.connect(root.followStates)
        }
    }
}
