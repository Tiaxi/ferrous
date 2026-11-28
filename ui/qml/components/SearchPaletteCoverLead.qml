// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami
import "." as Components
import "../logic/ColorUtils.js" as ColorUtils

// A search result's lead, in its palette's lead column (SearchPalette):
// the cover, or the library's no-cover tile, with the amber glyph over it
// while its track plays. The tile and the glyph are loaded only for the
// rows that show one, each with a source that never changes: a Kirigami
// icon looks its file up again on the GUI thread whenever its source
// changes, which recycled rows would do on every query.
Item {
    id: root
    required property Components.SearchPalette searchPalette
    // The row's cover, as a property of its row's own: rows the results
    // took away wait in the reuse pool, culled but not hidden, where a
    // row's `model` reads undefined, and showing the palette again had
    // their thumbnails read it (#772).
    property string coverUrl: ""
    // Whether the row shows a cover or the tile; a row without either
    // shows the glyph of its own in the lead column.
    property bool showsCover: true
    property bool isNow: false
    x: root.searchPalette.leadX
    y: Math.round((parent.height - height) / 2)
    width: root.searchPalette.leadSize
    height: root.searchPalette.leadSize

    Loader {
        active: root.showsCover && (root.coverUrl.length === 0 || thumbnail.failed)
        sourceComponent: noCoverTile
    }
    // A cover that has not loaded yet has the library's loading tint, so
    // that rows coming into view do not show an empty lead before their
    // covers; one that fails has the no-cover tile under it, as in the
    // library.
    Kirigami.ShadowedImage {
        id: thumbnail
        objectName: root.searchPalette.namePrefix + "Thumbnail"
        readonly property bool failed: status === Image.Error
        visible: root.showsCover && root.coverUrl.length > 0
        width: root.searchPalette.leadSize
        height: root.searchPalette.leadSize
        radius: 2
        // Wholly transparent rather than "transparent" once loaded or
        // failed, so the fill changes as a cover loads, which the
        // thumbnail repaints on (as the library's thumbnails).
        color: status === Image.Loading || status === Image.Null
            ? root.searchPalette.uiPalette.hover : ColorUtils.withAlpha(root.searchPalette.uiPalette.hover, 0)
        source: visible ? root.coverUrl : ""
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        sourceSize.width: Math.ceil(root.searchPalette.leadSize * Screen.devicePixelRatio)
        sourceSize.height: Math.ceil(root.searchPalette.leadSize * Screen.devicePixelRatio)
    }
    Rectangle {
        objectName: root.searchPalette.namePrefix + "NowScrim"
        visible: root.isNow
        width: root.searchPalette.leadSize
        height: root.searchPalette.leadSize
        radius: 2
        color: Qt.rgba(0, 0, 0, 0.55)
    }
    Loader {
        x: (root.searchPalette.leadSize - root.searchPalette.nowGlyphSize) / 2
        y: (root.searchPalette.leadSize - root.searchPalette.nowGlyphSize) / 2
        active: root.isNow
        sourceComponent: Kirigami.Icon {
            objectName: root.searchPalette.namePrefix + "NowGlyph"
            width: root.searchPalette.nowGlyphSize
            height: root.searchPalette.nowGlyphSize
            source: root.searchPalette.playbackPaused ? "media-playback-pause" : "media-playback-start"
            isMask: true
            roundToIconSize: false
            // Over the dark scrim on the cover or the tile, the colour
            // map's bright amber in both themes (fixed, like the plot's
            // colours): the light theme's darker amber is under 3:1 there.
            color: root.searchPalette.nowOnScrim
        }
    }
    Component {
        id: noCoverTile
        Components.NoCoverTile {
            objectName: root.searchPalette.namePrefix + "CoverPlaceholder"
            uiPalette: root.searchPalette.uiPalette
            size: root.searchPalette.leadSize
            // A thumbnail's here.
            radius: 2
        }
    }
}
