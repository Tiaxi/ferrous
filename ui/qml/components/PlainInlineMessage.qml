// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/PlainText.js" as PlainText

// Kirigami's inline message, showing its text as it is, never as markup.
// Set `message`, not `text`, as with PlainToolTip.
Kirigami.InlineMessage {
    id: root

    property string message: ""
    property bool labelIsPlain: false

    text: root.labelIsPlain ? root.message : ""

    Component.onCompleted: {
        PlainText.apply(root.contentItem)
        root.labelIsPlain = true
    }
}
