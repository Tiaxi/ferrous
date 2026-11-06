// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami

// The background of every dialog: the window colour (chrome), with the
// border, radius and shadow of the Breeze popup.
Kirigami.ShadowedRectangle {
    id: root

    required property var uiPalette

    radius: Kirigami.Units.cornerRadius
    color: root.uiPalette.chrome
    border.width: 1
    border.color: Kirigami.ColorUtils.linearInterpolation(
        root.uiPalette.chrome, root.uiPalette.text, Kirigami.Theme.frameContrast)
    shadow.xOffset: 0
    shadow.yOffset: 4
    shadow.size: 8
    shadow.color: Qt.rgba(0, 0, 0, 0.3)
}
