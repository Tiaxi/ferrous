// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "../logic/KeyUtils.js" as KeyUtils

// The button row of a dialog, in KDE's order whatever the desktop: the
// affirmative button, Apply, a destructive one and Cancel. Give each button
// its role with DialogButtonBox.buttonRole, and name the default one, which
// Breeze draws as such and Enter presses, as `primary`.
DialogButtonBox {
    id: root

    property Item primary: null

    function _applyPrimary() {
        // Qt 6.8 and later keep one default button for the box, and take
        // the mark from the others.
        if ("defaultButton" in root) {
            root.defaultButton = root.primary
        }
        if (root.primary) {
            root.primary.Accessible.defaultButton = true
        }
    }

    // The row's button with the keyboard, if one has it.
    function focusedButton() {
        for (let i = 0; i < root.contentChildren.length; ++i) {
            const button = root.contentChildren[i]
            if (button && button.activeFocus) {
                return button
            }
        }
        return null
    }

    // Enter, as KDE's dialogs take it: in a text field it only accepts the
    // text; on a button of the row it presses that button; anywhere else in
    // the dialog it presses the default button, if it can be pressed. A
    // combo box's open list and other popups take their keys before the
    // dialog sees them. Call it from a key handler of the dialog's content;
    // it accepts the Enter it took.
    function handleEnter(event, focusItem) {
        if (!KeyUtils.isEnter(event)) {
            return false
        }
        event.accepted = true
        if (KeyUtils.editsText(focusItem)) {
            return true
        }
        const button = root.focusedButton() || root.primary
        if (button && button.enabled && button.visible) {
            button.clicked()
        }
        return true
    }

    buttonLayout: DialogButtonBox.KdeLayout
    onPrimaryChanged: root._applyPrimary()
    Component.onCompleted: root._applyPrimary()
    Keys.onPressed: function(event) { root.handleEnter(event, Window.activeFocusItem) }
}
