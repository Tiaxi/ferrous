// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15

// Whether `popup` is on its way out: from the moment it starts to close, by
// an answer or otherwise, until it shows again. The app's style fades a
// popup out, and Qt still delivers clicks to it meanwhile: a button clicked
// again acts again, and a Dialog accepts again. Disable what acts while
// `closing` is set.
QtObject {
    id: root

    required property var popup
    readonly property bool closing: watcher.closing

    readonly property Connections watcher: Connections {
        property bool closing: false

        target: root.popup
        function onAboutToShow() { root.watcher.closing = false }
        function onAboutToHide() { root.watcher.closing = true }
    }
}
