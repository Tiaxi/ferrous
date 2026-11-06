// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// A heading inside a menu, such as "Repeat" over its choices: semibold text
// in the menu's text column, at the full text colour, low in its row so it
// sits with the items under it. It is not an item to pick: the pointer and
// the keyboard pass over it, and a click on it does nothing.
//
// It is a menu item, not a bare Item, so that a native menu (DBusMenu, the
// Plasma Global Menu) would show it as a labelled item rather than drop it.
// Qt 6.11 draws a MenuBar's menus itself on Linux, so today it never is.
MenuItem {
    id: root

    // Tells headings from the items they head, for tests and tools.
    readonly property bool isHeading: true

    objectName: "menuHeading"
    hoverEnabled: false
    focusPolicy: Qt.NoFocus
    checkable: false
    // The room above an item and a half; the text at the foot of the row.
    topPadding: Kirigami.Units.largeSpacing
    bottomPadding: Kirigami.Units.smallSpacing / 2
    font.weight: Font.DemiBold
    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
    // No Alt mnemonic: there is nothing to press.
    Kirigami.MnemonicData.enabled: false

    Accessible.role: Accessible.Heading
    Accessible.name: root.text

    // Takes every press, so the item never triggers or closes the menu.
    MouseArea {
        objectName: "menuHeadingPressSink"
        anchors.fill: parent
        z: 1
        acceptedButtons: Qt.AllButtons
    }
}
