// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// A menu item that shows, where a shortcut goes, the key that does the same
// in the view the menu was opened from: Enter plays the selected row, Delete
// removes it. An action's shortcut would take the key from the whole window
// (and Enter from the open menu), so the key is only shown here.
MenuItem {
    id: root

    // The key, as the menu shows a shortcut, such as "Enter" or "Del".
    property string keyHint: ""

    readonly property real hintSpace: hint.visible
        ? hint.implicitWidth + Kirigami.Units.largeSpacing
        : 0

    // The text gives the hint room: it ends where the hint begins.
    rightPadding: root.padding + (root.mirrored ? 0 : root.hintSpace)
    leftPadding: root.padding + (root.mirrored ? root.hintSpace : 0)

    Accessible.description: root.keyHint

    Label {
        id: hint
        objectName: "menuKeyHint"
        visible: root.keyHint.length > 0
        // Where Breeze puts a shortcut: a small spacing in from the padding.
        x: root.mirrored
            ? root.padding + Kirigami.Units.smallSpacing
            : root.width - root.padding - Kirigami.Units.smallSpacing - width
        y: root.topPadding + (root.availableHeight - height) / 2
        text: root.keyHint
        font: root.font
        color: root.pressed ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
    }
}
