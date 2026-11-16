// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami

// The background of a list row, shared by the library tree and the queue.
// Put it first in a row delegate, which it fills to the list's right edge,
// so a tint ends where the pane does, with a scrollbar or without; the
// list's overlay scrollbar draws its thumb over it (#373 H6). A list whose
// scrollbar is not an overlay leaves its lane with scrollbarLane.
//
// - hovered: the hover tint, which fades in and out: the main window's
//   library and queue set the stronger rowHover;
// - selected: the selection tint, or the inactive one while the list has
//   no keyboard focus;
// - focused (the current row of a list with keyboard focus, while the
//   keyboard is in use): a 1 px ring;
//   on a selected row, the tint and the ring are one rectangle; on an
//   unselected row, the ring alone, as in Breeze. A list whose scrollbar
//   lies over the rows' right end sets focusRingLane, and the ring stops
//   short of it, so the thumb never runs over the ring (#373 H12 Z12);
//   the tint goes on under the lane.
// Selection and focus change at once.
//
// - opaque: the row also paints the view colour under its tints, so a row
//   that slides over another while its list moves rows hides it.
Item {
    id: root

    required property var uiPalette
    property bool hovered: false
    // The hover tint: the window's, or a list's own.
    property color hoverColor: root.uiPalette.hover
    property bool selected: false
    // Whether the list has keyboard focus: its selection is the inactive
    // tint otherwise.
    property bool active: true
    property bool focused: false
    property bool opaque: false
    // The room on the right the row leaves free, for a scrollbar that
    // takes a lane of its own.
    property real scrollbarLane: 0
    // The room on the right the focus ring keeps clear of, for an overlay
    // scrollbar's lane while it shows.
    property real focusRingLane: 0
    readonly property real ringLane: root.focused ? root.focusRingLane : 0

    readonly property color tintColor: root.selected
        ? (root.active ? root.uiPalette.selection : root.uiPalette.selectionInactive)
        : "transparent"

    anchors.fill: parent
    anchors.rightMargin: root.scrollbarLane

    Rectangle {
        objectName: "rowBackgroundBase"
        anchors.fill: parent
        color: root.uiPalette.view
        visible: root.opaque
    }

    Rectangle {
        id: hoverTint
        objectName: "rowBackgroundHover"
        anchors.fill: parent
        color: root.hoverColor
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
        anchors.rightMargin: root.ringLane
        color: root.tintColor
        border.width: root.focused ? 1 : 0
        border.color: root.uiPalette.focusRing
        visible: root.selected || root.focused
    }

    // The tint under the lane the ring keeps clear of.
    Rectangle {
        objectName: "rowBackgroundTintLane"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.ringLane
        color: root.tintColor
        visible: root.selected && root.ringLane > 0
    }
}
