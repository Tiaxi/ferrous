// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// A title of the menu bar, as the style draws it, but lit as the lists
// show a selection: open, hovered or reached with the keyboard, it takes
// the selection tint with the text colour on it. The style draws white on
// a translucent accent there, 2.08:1 in Breeze Light, so the menu the user
// is in was the hardest word to read (#373 H4); this is 12.5:1 and 11:1.
//
// Only the label and the background are the app's. The style's item keeps
// its size and padding, its Alt mnemonics and its keyboard and pointer
// handling, and the menus are the style's.
MenuBarItem {
    id: root

    required property var uiPalette

    // Open, hovered or current: the menu bar marks it.
    readonly property bool lit: root.down || root.highlighted || root.hovered

    contentItem: Label {
        objectName: "menuBarItemLabel"
        // The title with the mnemonic underlined while Alt is held.
        text: root.Kirigami.MnemonicData.richTextLabel
        font: root.font
        color: root.enabled ? root.uiPalette.text : Kirigami.Theme.disabledTextColor
        elide: Text.ElideRight
        visible: root.text.length > 0
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        objectName: "menuBarItemBackground"
        // The style's.
        implicitWidth: 40
        implicitHeight: Kirigami.Units.gridUnit + 2 * Kirigami.Units.smallSpacing
        color: root.uiPalette.selection
        opacity: root.lit ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            enabled: Kirigami.Units.shortDuration > 0
            NumberAnimation {
                duration: Kirigami.Units.shortDuration
                easing.type: Easing.OutCubic
            }
        }
    }
}
