// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15

// The button row of a dialog whose buttons act through their own onClicked:
// the same row and order as DialogButtons, but a dialog does not accept,
// reject or close by the buttons' roles, which only place them.
Control {
    id: root

    default property alias buttons: box.contentData
    property alias primary: box.primary

    padding: 0

    contentItem: DialogButtons {
        id: box
    }
}
