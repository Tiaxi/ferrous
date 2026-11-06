// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// The help text under a control of a form: the base font in the secondary
// colour, which is at least 4.5:1 in both themes, wrapped at about 60
// characters: a wider text would turn the form to its narrow layout, with
// the labels above the fields, in a dialog of the default size. Under a
// check box it starts under the box's text: a form places its rows itself,
// so the indent is inside.
Item {
    id: root

    required property var uiPalette
    property alias text: label.text
    property alias color: label.color
    property alias font: label.font
    // How far right of the field column the text starts.
    property real indent: 0
    readonly property real maximumTextWidth: Math.round(helpFontMetrics.averageCharacterWidth * 60)

    implicitWidth: root.indent + Math.min(label.implicitWidth, root.maximumTextWidth)
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
