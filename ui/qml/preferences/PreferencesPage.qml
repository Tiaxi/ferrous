// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// A page of Configure Ferrous: its title on the content edge, then a form
// with right-aligned labels sized to the longest one. A page taller than
// the dialog scrolls.
ScrollView {
    id: root

    required property var uiPalette
    required property var settings
    property string title: ""
    // The rows of the form, with Kirigami.FormData.label set on each.
    default property alias formData: form.data

    // The page inset: 24 px at the sides, 16 px above the title.
    readonly property real pageSideInset: Kirigami.Units.gridUnit + Kirigami.Units.smallSpacing + 2
    readonly property real pageTopInset: Kirigami.Units.gridUnit - 2

    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    ColumnLayout {
        width: root.availableWidth
        spacing: 0

        Label {
            objectName: "preferencesPageTitle"
            Layout.fillWidth: true
            Layout.leftMargin: root.pageSideInset
            Layout.rightMargin: root.pageSideInset
            Layout.topMargin: root.pageTopInset
            Layout.bottomMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing + 2
            text: root.title
            // 15 px with the default 10 pt font.
            font.pointSize: root.uiPalette.basePointSize * 1.125
            font.weight: Font.DemiBold
            color: root.uiPalette.text
            elide: Text.ElideRight
            Accessible.role: Accessible.Heading
        }

        Kirigami.FormLayout {
            id: form
            objectName: "preferencesForm"
            // The form starts at the page inset, as wide as its longest
            // row, rather than centred.
            Layout.fillWidth: false
            Layout.preferredWidth: Math.min(implicitWidth, root.availableWidth - 2 * root.pageSideInset)
            Layout.leftMargin: root.pageSideInset
            Layout.rightMargin: root.pageSideInset
            Layout.bottomMargin: root.pageTopInset
        }
    }
}
