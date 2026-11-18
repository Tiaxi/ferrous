// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls

// A menu of the style's, as a window of its own, whose arrow keys pass over
// what cannot be picked: separators, headings and disabled items, as in KDE's
// other menus.
//
// A window popup can reach past the edges of the window it opens over, so a
// menu of a small main window or a small dialog is not cut off or squeezed
// in. Its submenus follow it. A platform without windows of their own keeps
// it an item in the window.
//
// The Breeze style turns on its item list's own key navigation, which sees
// Up and Down before the menu does and moves to the next row of any kind,
// so the keyboard stops on a separator or a heading with nothing
// highlighted. The menu's own navigation skips those rows.
Menu {
    id: root

    popupType: Popup.Window

    // A menu that is a window goes at once when it closes, without the
    // style's fade out; Plasma's compositor fades popup windows itself.
    //
    // On Wayland a menu window opened while another is still shown is made
    // that one's child, and goes when it goes. During the fade, the menu
    // the pointer moved to on the menu bar, a submenu next to another, or a
    // context menu opened while the last one closed, never showed.
    Binding on exit {
        when: root.popupType === Popup.Window
        value: null
    }

    // Opens the menu at a point of an item: where the pointer pressed, or
    // the corner of the row it was opened for from the keyboard.
    //
    // On Wayland the compositor places a window popup, and Qt gives it the
    // popup's parent item as the place to open at: under that item, or above
    // it when there is no room below. The point given to popup() is not
    // passed on, so a menu opened at the pointer in a long list opened under
    // the list. The menu's parent is therefore an item one pixel in size at
    // the point. It is hidden, so no layout or positioner places it, and it
    // is the item's child, so the menu takes the item's colours as before.
    function popupAt(item, x, y) {
        root.pointAnchor.parent = item
        root.pointAnchor.x = x
        root.pointAnchor.y = y
        root.popup(root.pointAnchor, 0, 0)
    }

    readonly property Item pointAnchor: Item {
        objectName: "appMenuPointAnchor"
        width: 1
        height: 1
        visible: false
    }

    Binding {
        target: root.contentItem
        property: "keyNavigationEnabled"
        value: false
        when: root.contentItem !== null && root.contentItem.keyNavigationEnabled !== undefined
    }
}
