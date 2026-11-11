// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// The room between two groups of a form's rows: 16 px from a group's last
// row to the next group's first, or to the next group's subheading, which
// is 8 px above its rows. The subheading is plain: the base font,
// semibold, where the fields start. A help text that ends a group leaves
// the room itself (FormHelpText.endsGroup), so no gap follows one. The
// subheading of a form's first group has no group above it: it starts
// the form, 8 px above its rows.
Item {
    id: root

    required property var uiPalette
    // The next group's subheading, if it has one.
    property string heading: ""
    // Whether it starts the form, with no group above it.
    property bool first: false

    // Kirigami.FormLayout's space between its rows.
    readonly property real rowSpacing: Kirigami.Units.smallSpacing
    readonly property real groupSpacing: Kirigami.Units.largeSpacing * 2
    readonly property real headingSpacing: Kirigami.Units.largeSpacing

    implicitWidth: root.heading.length > 0 ? headingLabel.implicitWidth : 0
    // The form puts its row spacing above and below this row.
    // The room above the subheading: none at the form's top, where the
    // form puts no row spacing either.
    readonly property real leadingSpace: root.first ? 0 : root.groupSpacing - root.rowSpacing
    implicitHeight: root.heading.length > 0
        ? root.leadingSpace + headingLabel.implicitHeight + root.headingSpacing - root.rowSpacing
        : root.groupSpacing - 2 * root.rowSpacing
    Accessible.ignored: root.heading.length === 0

    Label {
        id: headingLabel
        objectName: "formSubheading"
        y: root.leadingSpace
        width: Math.min(implicitWidth, root.width)
        visible: root.heading.length > 0
        text: root.heading
        font.weight: Font.DemiBold
        color: root.uiPalette.text
        elide: Text.ElideRight
        Accessible.role: Accessible.Heading
        Accessible.name: root.heading
    }
}
