// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// A page of Configure Ferrous: its title on the content edge, then a form
// with right-aligned labels sized to the longest one. A page taller than
// the dialog scrolls. The form takes the page's width: its labels beside
// the fields while the page has the room for both, above them when it has
// not, whatever the font; help texts wrap to the fields' column.
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
    // The form's width: the page's, less its insets.
    readonly property real formWidth: Math.max(0, root.availableWidth - 2 * root.pageSideInset)
    // What the form's rows need across. The labels' column: the widest
    // label, in the font Kirigami draws it in. The fields' column: the
    // widest field as it would be laid out, and at its narrowest; a help
    // text wraps, from about 20 characters. A row's own layout counts, not
    // whether it shows: a page out of view hides its rows.
    readonly property var formNeeds: {
        let labels = 0
        let fields = 0
        let narrowest = 0
        const children = form.children
        for (let i = 0; i < children.length; ++i) {
            const item = children[i]
            // The form's own grid, which holds the rows' places.
            if (item instanceof GridLayout) {
                continue
            }
            const label = item.Kirigami.FormData.label
            if (label && label.trim().length > 0) {
                labels = Math.max(labels, labelMetrics.advanceWidth(label))
            }
            if (item.formHelpText === true) {
                fields = Math.max(fields, item.indent + item.minimumTextWidth)
                continue
            }
            const preferred = item.Layout.preferredWidth > 0 ? item.Layout.preferredWidth : item.implicitWidth
            fields = Math.max(fields, item.Layout.minimumWidth,
                item.Layout.maximumWidth > 0 ? Math.min(preferred, item.Layout.maximumWidth) : preferred)
            narrowest = Math.max(narrowest, item.Layout.minimumWidth,
                item.Layout.fillWidth ? 0 : preferred)
        }
        return {
            wide: Math.ceil(labels) + Kirigami.Units.largeSpacing + Math.ceil(fields),
            narrow: Math.ceil(narrowest)
        }
    }
    // The page's room besides the form: its insets, and a scroll bar.
    readonly property real chromeWidth: 2 * root.pageSideInset + root.ScrollBar.vertical.width
    // The page's width for the form's wide layout, and its narrowest: the
    // labels go above the fields, and only the widest field counts.
    readonly property real wideWidth: Math.ceil(root.chromeWidth + root.formNeeds.wide)
    readonly property real minimumWidth: Math.ceil(root.chromeWidth + root.formNeeds.narrow)

    FontMetrics {
        id: labelMetrics
        font: Kirigami.Theme.defaultFont
    }

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
            // As wide as the page: its wide layout spreads the fields'
            // column to the page's edge, for the help texts to wrap in.
            // It has that layout while the labels and the fields fit side
            // by side, whatever the font.
            implicitWidth: root.formWidth
            wideMode: root.formWidth >= root.formNeeds.wide
            Layout.fillWidth: false
            Layout.preferredWidth: root.formWidth
            Layout.leftMargin: root.pageSideInset
            Layout.rightMargin: root.pageSideInset
            Layout.bottomMargin: root.pageTopInset
        }
    }
}
