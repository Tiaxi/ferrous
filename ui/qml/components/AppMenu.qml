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

    // The items the menu makes itself, such as a submenu's, are the app's
    // too: their words line up with the other items' at any font.
    delegate: AppMenuItem {}

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

    // A menu window shows at the size of its items as they are shown.
    //
    // Qt shows a menu's window, and only then the menu in it: the window
    // took the size of the menu as it was closed. Breeze sizes a menu from
    // its items as they are shown: hidden, an item is 0 high, a separator
    // has no height, a shortcut has no text, and the columns for check
    // marks and icons come when the items first show. So a menu window was
    // first a strip a few pixels high, which grew as its items showed. On
    // Wayland the compositor answers each size Qt asks for after Qt has
    // asked for the next: Qt took the strip's size back from it, the
    // strip's scroll bar made the menu wider, and Qt asked for that width
    // at the strip's height, which the compositor then kept. The menu
    // showed as an empty strip, opened on the menu bar by the pointer or
    // as a context menu.
    //
    // The window turns visible before the platform shows it, so the menu
    // shows itself then, and lays out its items' rows: an item lays out
    // its row when it is drawn, after the window has shown. The window then
    // takes the menu's size as it will be drawn, and the compositor is
    // asked for that size alone. A menu that is an item has no window of
    // its own to size.
    Connections {
        target: root.contentItem ? root.contentItem.Window.window : null
        enabled: root.popupType === Popup.Window

        function onVisibleChanged(visible) {
            if (visible)
                root.showWithWindow()
        }
    }

    function showWithWindow() {
        // The item that holds the menu, which Qt shows next.
        const menuItem = root.contentItem.parent
        if (menuItem !== null)
            menuItem.visible = true
        for (let i = 0; i < root.count; ++i) {
            const item = root.itemAt(i)
            if (item && item.contentItem)
                item.contentItem.ensurePolished()
        }
    }

    // Whether an item of the menu has both a check mark and a glyph, found
    // out as Breeze's menu finds out whether it has either, as its items
    // show; such a menu gives marks and glyphs a column each
    // (AppMenuItem). Kept once found, as Breeze keeps its, so a closed
    // menu is as wide as it shows.
    property bool hasMarkedGlyphs: false

    Connections {
        target: root.contentItem ? root.contentItem.contentItem : null

        function onVisibleChildrenChanged() {
            if (root.hasMarkedGlyphs)
                return
            for (const child of root.contentItem.contentItem.visibleChildren) {
                if (child.checkable && child.icon
                        && (child.icon.name !== "" || child.icon.source.toString() !== "")) {
                    root.hasMarkedGlyphs = true
                    return
                }
            }
        }
    }

    Binding {
        target: root.contentItem
        property: "keyNavigationEnabled"
        value: false
        when: root.contentItem !== null && root.contentItem.keyNavigationEnabled !== undefined
    }
}
