// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// A view that Tab reaches and the arrow, Page, Home and End keys scroll.
// Other keys, such as a viewer's Escape, go on.
ScrollView {
    id: root

    // How far the arrow keys scroll.
    property real lineStep: Kirigami.Units.gridUnit

    contentWidth: availableWidth
    clip: true
    activeFocusOnTab: true
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        color: "transparent"
        radius: Kirigami.Units.cornerRadius
        border.width: 2
        border.color: root.activeFocus ? Kirigami.Theme.focusColor : "transparent"
    }

    function scrollBy(pixels) {
        const flickable = root.contentItem
        const bottom = Math.max(0, flickable.contentHeight - flickable.height)
        flickable.contentY = Math.max(0, Math.min(bottom, flickable.contentY + pixels))
    }

    // Scrolls no further than it takes to show the content from top to bottom.
    function reveal(top, bottom) {
        const flickable = root.contentItem
        if (top < flickable.contentY) {
            root.scrollBy(top - flickable.contentY)
        } else if (bottom > flickable.contentY + flickable.height) {
            root.scrollBy(Math.min(top - flickable.contentY,
                bottom - flickable.contentY - flickable.height))
        }
    }

    Keys.onPressed: function(event) {
        const page = Math.max(root.lineStep, root.height - root.lineStep)
        const all = root.contentItem.contentHeight
        switch (event.key) {
        case Qt.Key_Up: root.scrollBy(-root.lineStep); break
        case Qt.Key_Down: root.scrollBy(root.lineStep); break
        case Qt.Key_PageUp: root.scrollBy(-page); break
        case Qt.Key_PageDown: root.scrollBy(page); break
        case Qt.Key_Home: root.scrollBy(-all); break
        case Qt.Key_End: root.scrollBy(all); break
        default:
            event.accepted = false
            return
        }
        event.accepted = true
    }
}
