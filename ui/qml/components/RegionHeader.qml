// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// The header row of a region, such as the library and the queue: its name,
// its totals ("12 artists · 28 albums · 176 tracks") and a hairline under
// it. Items declared inside go at its right end, such as column labels.
Item {
    id: root

    required property var uiPalette
    property string title: ""
    property string totals: ""
    // Where the name starts, such as the queue's title column.
    property real leftInset: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
    property real rightInset: 0
    default property alias trailingData: trailing.data

    implicitHeight: root.uiPalette.rowHeight
    implicitWidth: root.leftInset + titleLabel.implicitWidth + Kirigami.Units.largeSpacing
        + totalsLabel.implicitWidth + trailing.implicitWidth + root.rightInset

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
            text: root.title
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.uiPalette.smallPointSize
            font.weight: Font.DemiBold
            color: root.uiPalette.text
            elide: Text.ElideRight
            Layout.maximumWidth: implicitWidth
            Layout.fillWidth: true
            Accessible.ignored: true
        }

        Label {
            id: totalsLabel
            objectName: "regionHeaderTotals"
            text: root.totals
            visible: text.length > 0
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.uiPalette.smallPointSize
            font.features: root.uiPalette.tabularFigures
            color: root.uiPalette.textTertiary
            elide: Text.ElideRight
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            Accessible.ignored: true
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
