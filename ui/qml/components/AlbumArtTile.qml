// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Shapes
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

// The playing track's cover, edge to edge: no inset, radius or frame. A
// track without a cover has a stand-in in its place (#373 H11: the panel
// showed no tile for it, so a track change between one with a cover and
// one without moved the whole sidebar by the cover's height): the plot's
// floor colour with one static mark at its centre, the same for every
// track without a cover (#373 H14 F2: a stand-in of text repeated the
// lines under it, and without an album it was blank): the app icon's glide
// and its harmonics, at the icon's own aspect, at a cover mark's strength
// rather than data's. A small tile, as the narrow layout's 96 px one, has
// the mark of two curves. In a light theme a 1 px edge
// (`standInEdgeColor`) keeps the dark tile from reading as a hole in the
// sidebar. With nothing playing (`idle`) it shows only the placeholder
// colour with Breeze's audio disc mark at its centre (#373 H1, H3): the
// now-playing panel shows its compact row then, and the tile only as it
// shrinks into it.
//
// The tile is a keyboard stop while a track plays: a 1 px ring shows its
// focus while the keyboard is in use, and the Menu key or Shift+F10 opens
// its context menu. The pointer over a cover lays a faint veil on it, and
// a double-click opens it in the viewer; the stand-in has neither, as
// there is nothing to open.
Rectangle {
    id: root

    required property var uiBridge
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer
    // The tile and its mark while the track has no cover, and while
    // nothing plays: the now-playing panel passes the palette's.
    property color placeholderColor: "transparent"
    // Nothing playing: the tile shows no cover, only the placeholder.
    property bool idle: false
    // The stand-in for a track without a cover: its colour, the plot's
    // floor, and the 1 px edge around it, or transparent for none.
    property color standInColor: "transparent"
    property color standInEdgeColor: "transparent"
    // The mark's colours, as drawn on the floor: a cover's mark, not the
    // plot's data. The glide in the icon's amber as it was at 68 %, 5:1;
    // the harmonics in its magentas, a little brighter than at 68 %, some
    // 3.8:1 on the floor rather than 3.2 (#373 H16 I6).
    property color standInGlideColor: "#ab7a33"
    property color standInHarmonicColor: "#b6476e"
    property color standInHighHarmonicColor: "#b04698"
    // The mark's strokes, by the tile's size, so they weigh alike at every
    // size (#373 H16 I6: drawn from one file they were 1.6 to 3.2 px): the
    // glide's 2 px up to a 132 px tile, rising to 3 px at 360 and on; the
    // harmonics 0.8 and 0.65 of it on the wide mark, 0.75 on the small.
    readonly property real standInStroke: Math.min(3.5, 2 + Math.max(0, root.layoutSize - 132) / 228)
    readonly property bool standInShown: !root.idle && root.coverPath.length === 0
    // The tile's size once in place: the now-playing panel keeps it while
    // the tile grows or shrinks, so that the mark is neither drawn again
    // at each size nor changed for the small one on the way.
    property real layoutSize: Math.min(root.width, root.height)
    // A tile under ten grid units, such as the narrow layout's 96 px, has
    // the mark of two curves.
    readonly property bool standInCompact: root.layoutSize < 10 * Kirigami.Units.gridUnit
    // The veil the pointer lays on the tile, and the keyboard focus ring.
    property color hoverVeilColor: "transparent"
    property color focusRingColor: Kirigami.Theme.focusColor
    property color placeholderGlyphColor: Kirigami.Theme.textColor
    // The mark's size: 48 px, and a third of a smaller tile, unless the
    // now-playing panel sizes it from the font.
    property real placeholderGlyphSize: Math.min(Kirigami.Units.iconSizes.large,
        Math.round(Math.min(root.width, root.height) / 3))
    // A disc with a note, which says music rather than a missing picture,
    // as the album cover mark's frame did (#373 H6).
    readonly property string placeholderGlyphSource: "media-optical-audio-symbolic"
    // What shows around a cover that does not fill the tile, or through
    // one that is partly transparent: the colour behind the tile, which the
    // now-playing panel passes. Each cover draws on it, so a cover at full
    // opacity hides all under it, the covers fading out under it too.
    property color backdropColor: "transparent"
    // The cover it shows: the playing track's, unless the now-playing
    // panel shows another, as the one before while it shrinks away.
    property string coverPath: root.uiBridge.currentTrackCoverPath
    readonly property bool hasCover: !root.idle && root.coverPath.length > 0
    // Hovered by the pointer.
    readonly property bool hovered: tileMouseArea.containsMouse
    // The ring shows keyboard focus only, as in the lists (#373 H2).
    readonly property bool focusRingShown: focusTarget.activeFocus && InputModality.keyboard
    // What takes the keyboard's focus, the pointer and the keys.
    readonly property Item focusItem: focusTarget
    // The covers before, or their stand-in for none, over this one while
    // they fade out: the now-playing panel's album change. Oldest first,
    // each {coverPath, from}, showing at `from` times `outgoingLevel`, and
    // over the ones after it; one without a cover path is the stand-in.
    property var outgoingCovers: []
    property real outgoingLevel: 0
    // The most covers before that fade out at once.
    property int outgoingSlots: 0
    // The size its images are loaded at: the now-playing panel keeps it
    // while the tile grows or shrinks.
    property real coverSourceSize: Math.max(256, root.width)

    implicitWidth: 320
    implicitHeight: 320
    Layout.fillWidth: true
    Layout.preferredHeight: width

    color: root.hasCover ? root.backdropColor
        : (root.standInShown ? root.standInColor : root.placeholderColor)
    // The keyboard stop over it speaks for it.
    Accessible.ignored: true

    function openContextMenu(x, y) {
        albumArtMenu.popup(root, x, y)
    }

    Image {
        anchors.fill: parent
        visible: !root.idle
        source: root.coverPath
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        cache: true
        retainWhileLoading: true
        sourceSize.width: root.coverSourceSize
        sourceSize.height: root.coverSourceSize
    }

    Kirigami.Icon {
        objectName: "albumArtPlaceholderGlyph"
        anchors.centerIn: parent
        width: root.placeholderGlyphSize
        height: width
        source: root.placeholderGlyphSource
        roundToIconSize: false
        isMask: true
        color: root.placeholderGlyphColor
        visible: root.idle
    }

    // The stand-in's mark, centred, 0.55 of the tile wide and as high as
    // the icon's aspect makes it, and its edge: the app icon's glide and its
    // 2nd and 3rd harmonics (ferrous.svg's own curves, #373 H19: H14 to H18
    // stretched them to 2:1), rising 1 : 2 : 3 from a common start at the
    // lower left, or in a small tile the glide and its 3rd harmonic, as the
    // icon's small sizes draw them (#373 H14 F2). As in the icon they run
    // out to the mark's right bound, cut flush there, a crop of a
    // spectrogram rather than a chart's series ending in round caps (H19).
    // Drawn as shapes, so their strokes keep the width set for the tile's
    // size (standInStroke) as the curves scale to it. Assistive technology
    // hears "No album art" from the keyboard stop over it.
    component StandInMark: Item {
        id: standIn

        Shape {
            id: mark
            objectName: "albumArtStandInMark"
            // The paths' own box, at the icon's aspect: the icon's 64-unit
            // drawing from its traces' start to the tile's inner edge, with
            // 1.25 units around their centre lines, 240 units wide. The
            // paths run on past its right side, and the clip cuts them there.
            readonly property size box: Qt.size(240, 212)
            readonly property real glideStroke: root.standInStroke
            readonly property real harmonicStroke: root.standInStroke * 0.8
            readonly property real highHarmonicStroke: root.standInStroke * (root.standInCompact ? 0.75 : 0.65)
            anchors.centerIn: parent
            width: Math.round(standIn.width * 0.55)
            height: Math.round(width * mark.box.height / mark.box.width)
            clip: true
            preferredRendererType: Shape.CurveRenderer
            antialiasing: true
            Accessible.ignored: true

            // The paths' scale to the mark's size: their strokes do not scale.
            readonly property size pathScale: Qt.size(mark.width / mark.box.width, mark.height / mark.box.height)

            ShapePath {
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                scale: mark.pathScale
                strokeColor: root.standInHighHarmonicColor
                strokeWidth: mark.highHarmonicStroke
                PathSvg {
                    path: "M6.70 146.15 C8.26 143.83 9.81 141.62 11.37 139.48 C13.70 136.27 16.04 133.24 18.37 130.35 C22.26 125.54 26.15 121.12 30.03 116.99 C35.48 111.22 40.92 106.01 46.36 101.20 C54.14 94.32 61.92 88.25 69.69 82.69 C79.80 75.47 89.91 69.14 100.02 63.36 C112.46 56.25 124.91 50.00 137.35 44.29 C152.90 37.16 168.46 30.89 184.01 25.17 C202.67 18.31 221.34 12.25 240.00 6.70 C246.22 4.86 252.44 3.07 258.66 1.33"
                }
            }
            ShapePath {
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                scale: mark.pathScale
                strokeColor: root.standInCompact ? "transparent" : root.standInHarmonicColor
                strokeWidth: mark.harmonicStroke
                PathSvg {
                    path: "M6.70 175.64 C8.26 174.10 9.81 172.62 11.37 171.20 C13.70 169.06 16.04 167.04 18.37 165.11 C22.26 161.90 26.15 158.96 30.03 156.21 C35.48 152.36 40.92 148.89 46.36 145.68 C54.14 141.09 61.92 137.04 69.69 133.34 C79.80 128.53 89.91 124.30 100.02 120.45 C112.46 115.71 124.91 111.54 137.35 107.74 C152.90 102.99 168.46 98.81 184.01 95.00 C202.67 90.42 221.34 86.38 240.00 82.68 C246.22 81.45 252.44 80.26 258.66 79.10"
                }
            }
            ShapePath {
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                scale: mark.pathScale
                strokeColor: root.standInGlideColor
                strokeWidth: mark.glideStroke
                PathSvg {
                    path: "M6.70 205.14 C8.26 204.37 9.81 203.63 11.37 202.92 C13.70 201.85 16.04 200.84 18.37 199.87 C22.26 198.27 26.15 196.80 30.03 195.42 C35.48 193.50 40.92 191.76 46.36 190.16 C54.14 187.86 61.92 185.84 69.69 183.99 C79.80 181.58 89.91 179.47 100.02 177.54 C112.46 175.17 124.91 173.09 137.35 171.19 C152.90 168.81 168.46 166.72 184.01 164.82 C202.67 162.53 221.34 160.51 240.00 158.66 C246.22 158.04 252.44 157.45 258.66 156.87"
                }
            }
        }

        Rectangle {
            objectName: "albumArtStandInEdge"
            anchors.fill: parent
            visible: root.standInEdgeColor.a > 0
            color: "transparent"
            border.width: 1
            border.color: root.standInEdgeColor
        }
    }

    StandInMark {
        objectName: "albumArtStandIn"
        anchors.fill: parent
        visible: root.standInShown
    }

    // The covers before fade out over this one, which shows whole all
    // along, so the panel never shows through between them. Each keeps its
    // slot while it fades, so its image is not loaded again.
    Item {
        anchors.fill: parent

        Repeater {
            model: root.outgoingSlots

            delegate: Rectangle {
                id: outgoing

                required property int index
                readonly property var cover: root.outgoingCovers[outgoing.index] || null
                readonly property string coverPath: outgoing.cover !== null ? outgoing.cover.coverPath : ""

                objectName: "albumArtOutgoing"
                // The older over the newer.
                z: -outgoing.index
                anchors.fill: parent
                visible: outgoing.cover !== null && outgoing.opacity > 0
                opacity: outgoing.cover !== null ? outgoing.cover.from * root.outgoingLevel : 0
                color: outgoing.coverPath.length > 0 ? root.backdropColor : root.standInColor
                // A cover fades out as one, on its backdrop, and a stand-in
                // with its mark: without a layer the opacity would take
                // them apart, and the backdrop would show through the image.
                layer.enabled: outgoing.visible

                // The same size as the cover had, so the image cache still has it.
                Image {
                    anchors.fill: parent
                    source: outgoing.visible ? outgoing.coverPath : ""
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    asynchronous: true
                    cache: true
                    sourceSize.width: root.coverSourceSize
                    sourceSize.height: root.coverSourceSize
                }

                StandInMark {
                    objectName: "albumArtOutgoingStandIn"
                    anchors.fill: parent
                    visible: outgoing.cover !== null && outgoing.coverPath.length === 0
                }
            }
        }
    }

    // The keyboard stop, over the covers, with the pointer's veil, the
    // focus ring and the pointer's input. Disabled while nothing plays, as
    // there is nothing to show or open: out of the Tab order then, and it
    // gives up the focus if it has it. Only this item is disabled, so the
    // mark under it keeps its colour.
    Item {
        id: focusTarget
        objectName: "albumArtFocusTarget"
        anchors.fill: parent
        enabled: !root.idle
        activeFocusOnTab: true
        Accessible.role: Accessible.Graphic
        Accessible.name: root.hasCover
            ? I18n.i18nc("@info", "Album art")
            : I18n.i18nc("@info", "No album art")
        Accessible.focusable: true

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Menu
                    || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                root.openContextMenu(0, root.height)
                event.accepted = true
            }
        }

        // The pointer's veil, over the covers.
        Rectangle {
            objectName: "albumArtHoverVeil"
            anchors.fill: parent
            color: root.hoverVeilColor
            opacity: root.hovered && root.hasCover ? 1 : 0
            visible: opacity > 0

            Behavior on opacity {
                enabled: Kirigami.Units.shortDuration > 0
                NumberAnimation {
                    duration: Kirigami.Units.shortDuration
                    easing.type: Easing.OutCubic
                }
            }
        }

        // Keyboard focus: a 1 px ring inside the tile, which runs edge to
        // edge.
        Rectangle {
            objectName: "albumArtFocusRing"
            anchors.fill: parent
            visible: root.focusRingShown
            color: "transparent"
            border.width: 1
            border.color: root.focusRingColor
        }

        MouseArea {
            id: tileMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton

            onPressed: function(mouse) {
                if (mouse.button === Qt.RightButton) {
                    root.openContextMenu(mouse.x, mouse.y)
                }
            }

            onDoubleClicked: function(mouse) {
                if (mouse.button === Qt.LeftButton && root.hasCover) {
                    root.openAlbumArtViewer()
                }
            }
        }
    }

    Menu {
        id: albumArtMenu
        objectName: "albumArtMenu"

        MenuItem { action: root.replaceFromItunesAction }

        MenuItem {
            enabled: false
            visible: !root.replaceFromItunesAction.enabled
            text: root.currentTrackItunesArtworkDisabledReason()
        }
    }
}
