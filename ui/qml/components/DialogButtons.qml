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

    function _indexOfButton(button) {
        for (let i = 0; i < root.count; ++i) {
            if (root.itemAt(i) === button) {
                return i
            }
        }
        return -1
    }

    function _applyPrimary() {
        // Qt 6.11 added the box's defaultButton, which it highlights and
        // gives the focus. Its setter removes the button it takes the mark
        // from with removeItem(), which destroys that button, so the old
        // default leaves the row while the mark moves and then comes back.
        if ("defaultButton" in root) {
            const previous = root.defaultButton
            const index = previous && previous !== root.primary ? root._indexOfButton(previous) : -1
            if (index >= 0) {
                root.takeItem(index)
            }
            root.defaultButton = root.primary
            if (index >= 0) {
                root.insertItem(index, previous)
            }
        }
        // Breeze draws the button marked default as such; only the primary
        // one keeps the mark.
        for (let i = 0; i < root.count; ++i) {
            const button = root.itemAt(i)
            if (button) {
                button.Accessible.defaultButton = button === root.primary
            }
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
    // dialog sees them. Call it from a key handler of the dialog's content,
    // with whether the dialog is open: a text field's own Enter may have
    // closed it already, and the keyboard has then left the field. It
    // accepts the Enter it took.
    function handleEnter(event, focusItem, dialogOpen) {
        if (!KeyUtils.isEnter(event)) {
            return false
        }
        event.accepted = true
        if (dialogOpen === false || KeyUtils.editsText(focusItem)) {
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
