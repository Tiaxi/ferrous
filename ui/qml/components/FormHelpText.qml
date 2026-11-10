// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// The help text under a control of a form: the base font in the secondary
// colour, which is at least 4.5:1 in both themes. It wraps to the form's
// field column, at most about 60 characters wide. In the form's wide
// layout it asks for no more than about 20 characters, so that a long help
// text never turns the form to its narrow layout, with the labels above the
// fields: the labels and the widest field decide that. Under a check box it
// starts under the box's text: a form places its rows itself, so the
// indent is inside.
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
    readonly property real maximumTextWidth: Math.round(helpFontMetrics.averageCharacterWidth * 60)
    readonly property real minimumTextWidth: Math.round(helpFontMetrics.averageCharacterWidth * 20)
    // Whether the form it is in has its labels beside the fields. Kirigami's
    // form keeps its rows as its own children.
    readonly property bool inWideForm: !root.parent || root.parent.wideMode !== false
    readonly property real naturalTextWidth: Math.min(label.implicitWidth, root.maximumTextWidth)

    implicitWidth: root.indent + (root.inWideForm
        ? Math.min(root.naturalTextWidth, root.minimumTextWidth)
        : root.naturalTextWidth)
    implicitHeight: label.implicitHeight
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
