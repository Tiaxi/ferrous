// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// The help text under a control of a form: the base font in the secondary
// colour, which is at least 4.5:1 in both themes. It wraps to the form's
// field column, at most about 63 characters wide: 440 px at the default
// font. In the form's wide
// layout it asks for no more than about 20 characters, so that a long help
// text never turns the form to its narrow layout, with the labels above the
// fields: the labels and the widest field decide that. Under a check box it
// starts under the box's text: a form places its rows itself, so the
// indent is inside. It is 4 px under its control, the form's row spacing,
// and leaves 12 px to the next row, or 16 px when it ends a group of rows
// (see FormGroupGap).
Item {
    id: root

    required property var uiPalette
    property alias text: label.text
    property alias color: label.color
    property alias font: label.font
    // How far right of the field column the text starts.
    property real indent: 0
    // Marks the help texts of a form for the pages that measure its fields.
    readonly property bool formHelpText: true
    // Whether the next row starts another group.
    property bool endsGroup: false
    // Kirigami.FormLayout's space between its rows, which it puts under
    // this one too.
    readonly property real rowSpacing: Kirigami.Units.smallSpacing
    // The room under the text, besides the row spacing.
    property real trailingSpace: (root.endsGroup
        ? Kirigami.Units.largeSpacing * 2
        : Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing) - root.rowSpacing
    readonly property real maximumTextWidth: Math.round(helpFontMetrics.averageCharacterWidth * 63)
    readonly property real minimumTextWidth: Math.round(helpFontMetrics.averageCharacterWidth * 20)
    // Whether the form it is in has its labels beside the fields. Kirigami's
    // form keeps its rows as its own children.
    readonly property bool inWideForm: !root.parent || root.parent.wideMode !== false
    readonly property real naturalTextWidth: Math.min(label.implicitWidth, root.maximumTextWidth)

    implicitWidth: root.indent + (root.inWideForm
        ? Math.min(root.naturalTextWidth, root.minimumTextWidth)
        : root.naturalTextWidth)
    implicitHeight: label.implicitHeight + root.trailingSpace
    Layout.fillWidth: true
    Layout.maximumWidth: root.indent + root.maximumTextWidth
    Accessible.ignored: true

    Label {
        id: label
        objectName: "formHelpTextLabel"
        x: root.indent
        width: Math.max(0, root.width - root.indent)
        wrapMode: Text.Wrap
        color: root.uiPalette.textSecondary
    }

    FontMetrics {
        id: helpFontMetrics
        font: label.font
    }
}
