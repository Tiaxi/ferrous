// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15

// The button row of a dialog, in KDE's order whatever the desktop: the
// affirmative button, Apply, a destructive one and Cancel. Give each button
// its role with DialogButtonBox.buttonRole, and name the default one, which
// Breeze draws as such, as `primary`.
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

    buttonLayout: DialogButtonBox.KdeLayout
    onPrimaryChanged: root._applyPrimary()
    Component.onCompleted: root._applyPrimary()
}
