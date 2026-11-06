// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import "../logic/KeyUtils.js" as KeyUtils

// A combo box of a dialog. Closed and not editable, it leaves Enter to the
// dialog's default button, as a combo box of a KDE dialog does: the style's
// takes the key itself. Its open list, and an editable box's text, still
// take Enter first. The dialog's content provides handleDialogEnter(event,
// focusItem); see DialogButtons.handleEnter.
ComboBox {
    id: root

    Keys.onPressed: function(event) {
        if (!KeyUtils.isEnter(event) || root.editable || (root.popup && root.popup.visible)) {
            return
        }
        for (let item = root.parent; item; item = item.parent) {
            if (typeof item.handleDialogEnter === "function") {
                item.handleDialogEnter(event, root)
                return
            }
        }
    }
}
