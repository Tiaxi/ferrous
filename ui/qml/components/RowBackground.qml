// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ColorUtils.js" as ColorUtils

// The background of a list row, shared by the library tree and the queue.
// Put it first in a row delegate, which it fills to the list's right edge.
// While a scrollbar shows, a list sets scrollbarLane to its lane, also
// one that lies over the rows' right end: every state's rectangle, the
// hover, the tint and the ring, ends at the lane, and the thumb is alone
// in it. Without one, they all end where the pane does.
//
// - hovered: the hover tint, which fades in and out: the main window's
//   library and queue set the stronger rowHover;
// - selected: the selection tint, or the inactive one while the list has
//   no keyboard focus;
// - focused (the current row of a list with keyboard focus, while the
//   keyboard is in use): a 1 px ring;
//   on a selected row, the tint and the ring are one rectangle; on an
//   unselected row, the ring alone, as in Breeze. Both end at the lane:
//   the thumb over the ring broke its end, and the tint going on alone
//   under the lane was a tab on it.
// Selection and focus change at once.
//
// - selectedHover (the queue's): a selected row under the pointer steps
//   its selection tint toward the text colour, with the hover tint's own
//   fade, so it answers the pointer as the other rows do.
//
// - opaque: the row also paints the view colour under its tints and in
//   the lane, so a row that slides over another while its list moves rows
//   hides it.
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
    // The room on the right the row's states leave free, for a scrollbar's
    // lane while it shows.
    property real scrollbarLane: 0
    property bool selectedHover: false

    readonly property color selectionColor: root.active ? root.uiPalette.selection : root.uiPalette.selectionInactive
    // How far the selection has stepped toward its hovered colour: the
    // hover tint's opacity, which fades in and out.
    readonly property real selectionHoverLevel: root.selectedHover ? hoverTint.opacity : 0
    readonly property color tintColor: !root.selected
        ? "transparent"
        : (root.selectionHoverLevel > 0
            ? ColorUtils.mixColor(root.selectionColor,
                                  root.active ? root.uiPalette.selectionHovered
                                              : root.uiPalette.selectionInactiveHovered,
                                  root.selectionHoverLevel)
            : root.selectionColor)

    anchors.fill: parent
    anchors.rightMargin: root.scrollbarLane

    Rectangle {
        objectName: "rowBackgroundBase"
        anchors.fill: parent
        anchors.rightMargin: -root.scrollbarLane
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
        color: root.tintColor
        border.width: root.focused ? 1 : 0
        border.color: root.uiPalette.focusRing
        visible: root.selected || root.focused
    }
}
