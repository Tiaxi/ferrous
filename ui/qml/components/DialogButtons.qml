// SPDX-License-Identifier: GPL-3.0-or-later

import QtQml.Models
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/KeyUtils.js" as KeyUtils

// The button row of a dialog, in KDE's order whatever the desktop: the
// affirmative button, Apply, a destructive one and Cancel. Give each button
// its role with DialogButtonBox.buttonRole, and name the default one, which
// Breeze draws as such and Enter presses, as `primary`.
//
// The buttons sit on the dialogs' content inset, a grid unit from the
// window's sides and bottom, as the content is from its sides and top;
// the content's own inset under it is the room above them.
// Buttons that share the row with the box, such as Defaults, take its
// `inset` as their side margin and its bottomPadding under them, and its
// `glyphSize` as their glyphs' size.
//
// The buttons' glyphs are as large as their captions' font calls for,
// Kirigami's size for labels: 16 px with the default font, larger with a
// larger one, as the iTunes rows' (not 16 px beside 24 pt captions).
DialogButtonBox {
    id: root

    property Item primary: null
    readonly property real inset: Kirigami.Units.gridUnit
    readonly property real glyphSize: Kirigami.Units.iconSizes.sizeForLabels

    leftPadding: root.inset
    rightPadding: root.inset
    bottomPadding: root.inset
    topPadding: 0

    // The row is as tall as its buttons: the style's list is 32 px tall
    // whatever they are, which left the room under them larger than the
    // inset where they are shorter.
    Binding {
        target: root.contentItem
        property: "implicitHeight"
        value: {
            let tallest = 0
            for (let i = 0; i < root.contentChildren.length; ++i) {
                const button = root.contentChildren[i]
                if (button && button.visible) {
                    tallest = Math.max(tallest, button.implicitHeight)
                }
            }
            return tallest
        }
        when: root.contentItem !== null && root.contentChildren.length > 0
    }

    // Each button's glyph, at the row's size. The bindings restore nothing
    // as they go: _applyPrimary takes a button out and back, and the old
    // binding, deleted after the new one was set, put 16 px back.
    Instantiator {
        model: root.contentChildren
        delegate: Binding {
            required property Item modelData
            target: modelData
            property: "icon.width"
            value: root.glyphSize
            restoreMode: Binding.RestoreNone
        }
    }

    Instantiator {
        model: root.contentChildren
        delegate: Binding {
            required property Item modelData
            target: modelData
            property: "icon.height"
            value: root.glyphSize
            restoreMode: Binding.RestoreNone
        }
    }

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
