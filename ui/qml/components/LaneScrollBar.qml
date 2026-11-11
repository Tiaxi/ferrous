// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// The scrollbar of the library tree and the queue: it lies over the 8 px
// lane on the right that the rows' text keeps clear of, drawing its thumb
// over their tints, which run to the edge, and shows only while the list is
// longer than its view. A 4 px thumb that widens under the pointer.
ScrollBar {
    id: root

    required property var uiPalette
    readonly property real lane: Kirigami.Units.largeSpacing
    readonly property bool engaged: root.hovered || root.pressed

    policy: ScrollBar.AsNeeded
    // AsNeeded shows it only when the content is larger than the view; it
    // stays shown rather than fading out, so the lane is never a surprise.
    visible: root.size < 1.0 && root.policy !== ScrollBar.AlwaysOff
    interactive: true
    implicitWidth: root.lane
    padding: 0
    minimumSize: Math.min(1, Kirigami.Units.gridUnit * 2 / Math.max(1, root.height))

    background: Item {}

    contentItem: Item {
        implicitWidth: root.lane

        Rectangle {
            objectName: "laneScrollBarThumb"
            anchors.right: parent.right
            anchors.rightMargin: root.engaged ? 1 : 2
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: root.engaged ? root.lane - 2 : root.lane / 2
            radius: width / 2
            color: Qt.rgba(root.uiPalette.text.r, root.uiPalette.text.g, root.uiPalette.text.b,
                           root.pressed ? 0.45 : (root.engaged ? 0.35 : 0.22))
        }
    }
}
