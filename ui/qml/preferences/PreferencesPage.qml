// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "." as Preferences

// A page of Configure Ferrous: its title on the content edge, then a form
// with right-aligned labels. Their column is as wide on every page, from
// the widest label of them all, so the labels end and the fields start at
// one place as the pages change. A page taller than the dialog scrolls.
// The form takes the page's width: its labels beside the fields while the
// page has the room for both, above them when it has not, whatever the
// font, and while its fields' column holds what the page needs to read;
// help texts wrap to the fields' column. Its form keeps its height
// while the page is out of view, so Configure can open tall enough for
// every page.
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
    // The widest label of this page's form, in the font its labels are
    // drawn in. A row's own label counts, not whether it shows.
    readonly property real labelWidth: {
        let labels = 0
        const rows = form.rows
        for (let i = 0; i < rows.length; ++i) {
            const label = rows[i].Kirigami.FormData.label
            if (label && label.trim().length > 0) {
                labels = Math.max(labels, labelMetrics.advanceWidth(label))
            }
        }
        return labels
    }
    // The widest label of every page, which Configure gives each page.
    property real sharedLabelWidth: 0
    // The labels' column: as wide as on the other pages, or wider for a
    // label of this page's own, so the labels end where the other pages' do.
    readonly property real labelColumnWidth: Math.max(root.labelWidth, root.sharedLabelWidth)
    // What the form's rows need across. The labels' column, and the
    // fields': the widest field as it would be laid out, and at its
    // narrowest; a help text wraps, from about 20 characters. A row's own
    // layout counts, not whether it shows.
    readonly property var formNeeds: {
        let fields = 0
        let narrowest = 0
        const rows = form.rows
        for (let i = 0; i < rows.length; ++i) {
            const item = rows[i]
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
            wide: Math.ceil(root.labelColumnWidth) + Kirigami.Units.largeSpacing + Math.ceil(fields),
            narrow: Math.ceil(narrowest)
        }
    }
    // What a page's fields need to be read in their column beside the
    // labels, such as Library's paths, where that is more than they are
    // laid out at: in less, the labels go above them. It widens no window.
    property real fieldsReadingWidth: 0
    // That column's width. Not from the form's, which a scroll bar that
    // comes and goes with the layout narrows.
    readonly property real fieldsColumnWidth: root.width - 2 * root.pageSideInset
        - Math.ceil(root.labelColumnWidth) - Kirigami.Units.largeSpacing
    // The page's room besides the form: its insets, and a scroll bar.
    readonly property real chromeWidth: 2 * root.pageSideInset + root.ScrollBar.vertical.width
    // The page's width for the form's wide layout, and its narrowest: the
    // labels go above the fields, and only the widest field counts.
    readonly property real wideWidth: Math.ceil(root.chromeWidth + root.formNeeds.wide)
    readonly property real minimumWidth: Math.ceil(root.chromeWidth + root.formNeeds.narrow)
    // The page's height with every row in view, whether or not it is.
    readonly property real wholeHeight: Math.ceil(pageContent.implicitHeight + root.topPadding + root.bottomPadding)

    FontMetrics {
        id: labelMetrics
        font: Kirigami.Theme.defaultFont
    }

    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    ColumnLayout {
        id: pageContent
        width: root.availableWidth
        spacing: 0

        Label {
            objectName: "preferencesPageTitle"
            Layout.fillWidth: true
            Layout.leftMargin: root.pageSideInset
            Layout.rightMargin: root.pageSideInset
            Layout.topMargin: root.pageTopInset
            // 12 px, which keeps the rows about where the smaller title had
            // them, and every page whole in the window as it opens.
            Layout.bottomMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
            textFormat: Text.PlainText
            text: root.title
            // 17 px with the default 10 pt font, over the groups' semibold
            // 13 px (at 15 px, only their x told the two ranks apart).
            font.pointSize: root.uiPalette.basePointSize * 1.3
            font.weight: Font.DemiBold
            color: root.uiPalette.text
            elide: Text.ElideRight
            Accessible.role: Accessible.Heading
        }

        Preferences.PreferencesForm {
            id: form
            // As wide as the page: its wide layout spreads the fields'
            // column to the page's edge, for the help texts to wrap in.
            // It has that layout while the labels and the fields fit side
            // by side, whatever the font, and the fields can be read there.
            wideMode: root.formWidth >= root.formNeeds.wide
                && root.fieldsColumnWidth >= root.fieldsReadingWidth
            labelColumnWidth: root.labelColumnWidth
            Layout.fillWidth: false
            Layout.preferredWidth: root.formWidth
            Layout.leftMargin: root.pageSideInset
            Layout.rightMargin: root.pageSideInset
            Layout.bottomMargin: root.pageTopInset
        }
    }
}
