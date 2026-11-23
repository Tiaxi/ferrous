// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// Why a row of a list is marked unreadable, for the queue and the library:
// the style's tooltip, from the row's title, below the row where it fits in
// the view and above it only near the view's bottom, so it covers the
// rows after it rather than the ones the eye came down (above, it hid
// every title over a queue's failed last row). It never covers the row
// that is now while one side is clear of it: above the view's top, over
// its header, if need be, as there is always chrome over the lists, and
// below its bottom only for want of that. It is as wide as its text up to
// 66 average characters of its font before it wraps, a line or two for a
// typical reason (the style wrapped at 14 grid units, which do not grow
// with the font: three lines at any size), and no wider than the row
// leaves it.
PlainToolTip {
    id: root

    // The row, which it belongs to.
    required property Item row
    // The row's top in the view, and the view's height.
    property real rowTop: 0
    property real viewHeight: 0
    // From its left end to the end of the row's states, where it ends at
    // the latest.
    property real room: Infinity
    // Whether the row that is now lies in part between `top` and `bottom`,
    // relative to the row's top: its tooltip leaves it clear if it can.
    property var coversNow: function(top, bottom) { return false }

    // As far from the row as the style's tooltip is from its control.
    readonly property real gap: 3
    readonly property real belowY: root.row.height + root.gap
    readonly property real aboveY: -root.gap - root.height
    readonly property bool fitsBelow: root.rowTop + root.belowY + root.height <= root.viewHeight
    readonly property bool fitsAbove: root.rowTop + root.aboveY >= 0
    readonly property bool opensBelow: {
        const clearBelow = !root.coversNow(root.belowY, root.belowY + root.height)
        const clearAbove = !root.coversNow(root.aboveY, root.aboveY + root.height)
        if (root.fitsBelow && clearBelow) {
            return true
        }
        if (clearAbove) {
            return false
        }
        if (clearBelow) {
            return true
        }
        return root.fitsBelow || !root.fitsAbove
    }
    readonly property real maximumTextWidth: Math.max(Math.ceil(reasonMetrics.averageCharacterWidth * 16),
        Math.min(Math.ceil(reasonMetrics.averageCharacterWidth * 66),
            root.room - root.leftPadding - root.rightPadding))

    parent: row
    y: root.opensBelow ? root.belowY : root.aboveY
    delay: Kirigami.Units.toolTipDelay
    contentWidth: Math.min(Math.ceil(reasonLabel.implicitWidth), root.maximumTextWidth)

    FontMetrics {
        id: reasonMetrics
        font: root.font
    }

    contentItem: Label {
        id: reasonLabel
        objectName: "rowReasonLabel"
        textFormat: Text.PlainText
        text: root.text
        font: root.font
        // Wrap rather than WordWrap, as the style's: a long path breaks
        // rather than running out of the tooltip.
        wrapMode: Text.Wrap
    }
}
