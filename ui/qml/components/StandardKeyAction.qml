// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import FerrousUi 1.0

// An action with every binding of a standard key, such as Ctrl+O and the
// Open key of Open (StandardKeys.h). Given a standard key, an action takes
// only its first binding, and the menu item that shows it warns of the
// rest. This one takes the first as its shortcut, which its menu item
// shows; StandardKeyShortcuts take the rest in the main window, and
// ActionShortcuts all of them in a viewer.
Action {
    // A StandardKey.
    property int standardKey: StandardKey.UnknownKey
    // Its bindings, the platform's first first.
    readonly property var keyBindings: StandardKeys.bindings(standardKey)

    shortcut: keyBindings.length > 0 ? keyBindings[0] : ""
}
