// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// "No cover" at a list's size, in the library and the search: a tile of the
// plot's floor with a 1 px edge, in a thumbnail's place and at its size, so
// it reads as a cover's place and not as a load that failed, with the
// now-playing stand-in's mark in one quiet colour (no-art-mark.svg). In a
// light theme the tile is light indigo, its rim and mark darker
// (UiPalette.noCoverTile).
Rectangle {
    id: root

    required property var uiPalette
    required property int size

    width: root.size
    height: root.size
    color: root.uiPalette.noCoverTile
    border.width: 1
    border.color: root.uiPalette.noCoverTileEdge

    // The 20 px mark is drawn on that tile's pixels, whose steps scale into
    // a staircase, so a larger tile has the mark drawn in curves. The
    // source never changes as a row is recycled, which would load it again.
    Image {
        objectName: "noCoverTileMark"
        anchors.fill: parent
        source: root.uiPalette.themeIsDark
            ? (root.size === 20 ? Qt.resolvedUrl("../assets/no-art-mark-20.svg")
                                : Qt.resolvedUrl("../assets/no-art-mark.svg"))
            : (root.size === 20 ? Qt.resolvedUrl("../assets/no-art-mark-20-light.svg")
                                : Qt.resolvedUrl("../assets/no-art-mark-light.svg"))
        sourceSize.width: root.size
        sourceSize.height: root.size
        // Crisp on the grid it is drawn for.
        smooth: root.size !== 20
        cache: true
    }
}
