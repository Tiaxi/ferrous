// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// A file's or a folder's path that the user reads but does not edit: one
// line of text, elided in the middle where it is too long, so the start
// and the name at the end stay, with the whole path in a tooltip. Not a
// read-only text field, which looks editable.
Label {
    id: root

    property string path: ""
    // The tooltip's text: the whole path, while the line cuts it short.
    readonly property string toolTipText: root.truncated ? root.path : ""

    text: root.path
    textFormat: Text.PlainText
    elide: Text.ElideMiddle
    maximumLineCount: 1
    wrapMode: Text.NoWrap
    Accessible.name: root.path

    HoverHandler {
        id: hover
    }

    ToolTip.visible: hover.hovered && root.toolTipText.length > 0
    ToolTip.delay: Kirigami.Units.toolTipDelay
    ToolTip.text: root.toolTipText
}
