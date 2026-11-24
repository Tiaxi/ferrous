// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/FormatUtils.js" as FormatUtils

// The header row of a region, such as the library and the queue: its name,
// its totals ("12 artists · 28 albums · 176 tracks") and a hairline under
// it. Items declared inside go at its right end, such as column labels.
// When the row is short of room, the totals give way to compactTotals, when
// the region has one, such as the library's "176 tracks"; without one they
// drop whole totals from the end ("12 artists · 28 albums"), never a part of
// one. They go once even that doesn't fit; the name elides only when it alone
// doesn't fit.
Item {
    id: root

    required property var uiPalette
    property string title: ""
    property string totals: ""
    // What the totals shorten to when they don't fit whole, at any width.
    property string compactTotals: ""
    // Where the name starts, such as the queue's title column.
    property real leftInset: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
    property real rightInset: 0
    default property alias trailingData: trailing.data

    implicitHeight: root.uiPalette.rowHeight
    implicitWidth: root.leftInset + titleLabel.implicitWidth + Kirigami.Units.largeSpacing
        + totalsLabel.implicitWidth + trailing.implicitWidth + root.rightInset

    // The room left to the totals, after the name, its 8 px gap and the
    // items at the right end.
    readonly property real totalsRoom: root.width - root.leftInset - root.rightInset
        - Math.ceil(titleLabel.implicitWidth) - Kirigami.Units.largeSpacing - trailing.implicitWidth
    // The totals when they fit whole in their room; otherwise the compact
    // totals, or as many of the totals, from the first, as fit whole; none
    // when even that doesn't fit.
    readonly property string shownTotals: {
        const fits = text => Math.ceil(totalsMetrics.advanceWidth(text)) <= root.totalsRoom
        if (root.compactTotals.length > 0 && root.totals.length > 0 && !fits(root.totals)) {
            return fits(root.compactTotals) ? root.compactTotals : ""
        }
        const parts = root.totals.length > 0 ? root.totals.split(FormatUtils.fieldSeparator) : []
        let shown = ""
        for (let i = 0; i < parts.length; ++i) {
            const candidate = i === 0 ? parts[0] : shown + FormatUtils.fieldSeparator + parts[i]
            if (!fits(candidate)) {
                break
            }
            shown = candidate
        }
        return shown
    }
    readonly property bool totalsShown: root.shownTotals.length > 0

    FontMetrics {
        id: totalsMetrics
        font: totalsLabel.font
    }

    Accessible.role: Accessible.Heading
    Accessible.name: root.totals.length > 0 ? root.title + ", " + root.totals : root.title

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.leftInset
        anchors.rightMargin: root.rightInset
        anchors.bottomMargin: hairline.height
        spacing: 0

        Label {
            id: titleLabel
            objectName: "regionHeaderTitle"
            textFormat: Text.PlainText
            text: root.title
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.uiPalette.smallPointSize
            font.weight: Font.DemiBold
            color: root.uiPalette.text
            elide: Text.ElideRight
            // Whole pixels: the layout rounds the width it gives, and a
            // fraction short of the text's width elides it. The totals give
            // up their room first.
            Layout.maximumWidth: Math.ceil(implicitWidth)
            Layout.preferredWidth: Math.ceil(implicitWidth)
            Layout.minimumWidth: Math.max(0, Math.min(Math.ceil(implicitWidth),
                root.width - root.leftInset - root.rightInset - trailing.implicitWidth))
            Layout.fillWidth: true
            Accessible.ignored: true
        }

        Label {
            id: totalsLabel
            objectName: "regionHeaderTotals"
            textFormat: Text.PlainText
            text: root.shownTotals
            visible: root.totalsShown
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.uiPalette.smallPointSize
            font.features: root.uiPalette.tabularFigures
            color: root.uiPalette.textTertiary
            elide: Text.ElideRight
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.minimumWidth: 0
            Layout.fillWidth: true
            Accessible.ignored: true
        }

        // The totals' room when they are gone, which keeps the items
        // declared inside at the right end.
        Item {
            visible: !totalsLabel.visible
            Layout.fillWidth: true
        }

        RowLayout {
            id: trailing
            spacing: 0
            Layout.fillHeight: true
        }
    }

    Rectangle {
        id: hairline
        objectName: "regionHeaderHairline"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: root.uiPalette.hairline
    }
}
