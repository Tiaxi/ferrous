// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

// Gives `item`, inside `popup`, the keyboard each time the popup shows. Qt
// gives it to the popup itself, and a key goes from the item that has the
// keyboard to that item's parents, never into its children: a dialog's
// content, whose Enter presses the default button, takes no key until the
// keyboard is in it. Tab goes on from `item` to the popup's controls, and
// Escape reaches the popup as before.
QtObject {
    id: root

    required property var popup
    required property Item item

    readonly property Connections watcher: Connections {
        target: root.popup
        // From the moment the popup shows, before it has faded in: a key
        // pressed as it appears reaches `item`.
        function onVisibleChanged() {
            if (root.popup.visible && root.item) {
                root.item.forceActiveFocus(Qt.PopupFocusReason)
            }
        }
    }
}
