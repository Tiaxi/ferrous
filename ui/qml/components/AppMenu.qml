// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15

// A menu of the style's, whose arrow keys pass over what cannot be picked:
// separators, headings and disabled items, as in KDE's other menus.
//
// The Breeze style turns on its item list's own key navigation, which sees
// Up and Down before the menu does and moves to the next row of any kind,
// so the keyboard stops on a separator or a heading with nothing
// highlighted. The menu's own navigation skips those rows.
Menu {
    id: root

    Binding {
        target: root.contentItem
        property: "keyNavigationEnabled"
        value: false
        when: root.contentItem !== null && root.contentItem.keyNavigationEnabled !== undefined
    }
}
