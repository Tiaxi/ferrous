// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami

// The background of a list row, shared by the library tree and the queue.
// Put it first in a row delegate, which it fills except for the scrollbar
// lane on the right: rows never cover the scrollbar.
//
// - hovered: the hover tint, which fades in and out;
// - selected: the selection tint, or the inactive one while the list has
//   no keyboard focus;
// - focused (the current row of a list with keyboard focus): a 1 px ring;
//   on a selected row, the tint and the ring are one rectangle; on an
//   unselected row, the ring alone, as in Breeze.
// Selection and focus change at once.
Item {
    id: root

    required property var uiPalette
    property bool hovered: false
    property bool selected: false
    // Whether the list has keyboard focus: its selection is the inactive
    // tint otherwise.
    property bool active: true
    property bool focused: false
    // The scrollbar lane on the right, which the row leaves free.
    property real scrollbarLane: Kirigami.Units.largeSpacing

    readonly property color tintColor: root.selected
        ? (root.active ? root.uiPalette.selection : root.uiPalette.selectionInactive)
        : "transparent"

    anchors.fill: parent
    anchors.rightMargin: root.scrollbarLane

    Rectangle {
        id: hoverTint
        objectName: "rowBackgroundHover"
        anchors.fill: parent
        color: root.uiPalette.hover
        opacity: root.hovered ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            enabled: Kirigami.Units.shortDuration > 0
            NumberAnimation {
                duration: Kirigami.Units.shortDuration
                easing.type: Easing.OutCubic
            }
        }
    }

    Rectangle {
        id: tint
        objectName: "rowBackgroundTint"
        anchors.fill: parent
        color: root.tintColor
        border.width: root.focused ? 1 : 0
        border.color: root.uiPalette.focusRing
        visible: root.selected || root.focused
    }
}
