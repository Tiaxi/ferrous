// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import FerrousUi 1.0

// An action whose shortcuts the user can change. Its default bindings are
// its own, or every binding of a standard key, such as Ctrl+O and the Open
// key of Open (StandardKeys.h), which follow Plasma's Shortcuts settings.
// The user's, from the settings, replace them, and may be none.
//
// Given a standard key, an action takes only its first binding, and the
// menu item that shows it warns of the rest. This one takes the first of
// its bindings as its shortcut, which its menu item shows;
// AlternateShortcuts take the rest in the main window, and ActionShortcuts
// all of them in a viewer.
Action {
    // The name the settings keep the user's shortcuts under, in snake case:
    // play_pause. It must not change once released.
    property string shortcutId: ""
    // A StandardKey whose bindings are the defaults, the platform's first
    // first; or
    property int standardKey: StandardKey.UnknownKey
    // the default bindings themselves, as portable text: ["Ctrl+Shift+Up"].
    property var defaultKeyBindings: StandardKeys.bindings(standardKey)
    // The shortcuts the user changed, by shortcutId, as
    // BridgeClient.keyboardShortcuts has them.
    property var overrides: ({})
    // Its name in Configure Keyboard Shortcuts, for an action whose text
    // follows what it does now, such as Play and Pause; its text otherwise.
    property string shortcutText: ""

    // Whether the user changed this action's shortcuts.
    readonly property bool hasCustomKeyBindings: shortcutId.length > 0 && !!overrides
        && overrides[shortcutId] !== undefined
    // Its bindings, as portable text: the user's, or the defaults.
    readonly property var keyBindings: hasCustomKeyBindings
        ? StandardKeys.splitSequences(String(overrides[shortcutId]))
        : defaultKeyBindings

    shortcut: keyBindings.length > 0 ? keyBindings[0] : ""
}
