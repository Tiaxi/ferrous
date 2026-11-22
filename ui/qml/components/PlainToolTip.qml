// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import "../logic/PlainText.js" as PlainText

// A tooltip of the style's that shows its text as it is, never as markup.
// Set `message`, not `text`: the text waits until the style's label is
// plain, as that label would lay out markup as soon as it is made.
ToolTip {
    id: root

    property string message: ""
    property bool labelIsPlain: false

    text: root.labelIsPlain ? root.message : ""

    Component.onCompleted: {
        PlainText.apply(root.contentItem)
        root.labelIsPlain = true
    }
}
