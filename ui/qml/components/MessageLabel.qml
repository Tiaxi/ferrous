// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import "../logic/BreakableText.js" as BreakableText

// A message that wraps and may hold a path or a system error, such as
// "failed to create temporary file in /…/read-only: Permission denied (os
// error 13)": its lines break inside the path only between folders, and
// never inside the error's code. Set `message`, not `text`. Its accessible
// name keeps the message as it is.
Label {
    id: root

    property string message: ""

    // The message as shown at the label's width. Set as the width changes
    // rather than bound to it: until a layout gives the label its width,
    // the width is the text's own, which the text would change as it is
    // written. The characters it adds take no room, so the width it then
    // takes leaves the text as it is.
    function updateText() {
        root.text = BreakableText.breakableMessage(root.message, function(name) {
            return messageMetrics.advanceWidth(name) <= root.width
        })
    }

    textFormat: Text.PlainText
    wrapMode: Text.Wrap
    Accessible.name: root.message

    onMessageChanged: root.updateText()
    onWidthChanged: root.updateText()
    onFontChanged: root.updateText()
    Component.onCompleted: root.updateText()

    FontMetrics {
        id: messageMetrics
        font: root.font
    }
}
