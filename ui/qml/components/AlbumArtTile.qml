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
// and its harmonics, two thirds of the tile wide, at a cover mark's
// strength rather than data's. A small tile, as the narrow layout's 96 px
// one, has the mark of two curves. In a light theme a 1 px edge
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
    // the mark of two curves, drawn for its size.
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

    // The stand-in's mark, centred, two thirds of the tile wide and half
    // as high, and its edge: the app icon's glide and its 2nd and 3rd
    // harmonics (ferrous.svg's own curves, stretched to 2:1), rising
    // 1 : 2 : 3 from a common start at the lower left, or in a small tile
    // the glide and its 3rd harmonic, as the icon's small sizes draw them
    // (#373 H14 F2); drawn as shapes, so their strokes keep the width set
    // for the tile's size (standInStroke) as the curves scale to it.
    // Assistive technology hears "No album art" from the keyboard stop over
    // it.
    component StandInMark: Item {
        id: standIn

        Shape {
            id: mark
            objectName: "albumArtStandInMark"
            // The paths' own box: 240 by 120, or the small mark's 64 by 32.
            readonly property size box: root.standInCompact ? Qt.size(64, 32) : Qt.size(240, 120)
            readonly property real glideStroke: root.standInStroke
            readonly property real harmonicStroke: root.standInStroke * 0.8
            readonly property real highHarmonicStroke: root.standInStroke * (root.standInCompact ? 0.75 : 0.65)
            anchors.centerIn: parent
            width: Math.round(standIn.width * 2 / 3)
            height: Math.round(width / 2)
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
                    path: root.standInCompact
                        ? "M1.50 21.88 C1.91 21.54 2.31 21.22 2.72 20.91 C3.33 20.44 3.94 19.99 4.55 19.57 C5.57 18.87 6.58 18.22 7.60 17.61 C9.02 16.78 10.45 16.02 11.87 15.31 C13.90 14.31 15.94 13.41 17.97 12.61 C20.61 11.55 23.26 10.62 25.90 9.78 C29.15 8.74 32.41 7.83 35.66 6.99 C39.73 5.95 43.79 5.03 47.86 4.20 C52.74 3.19 57.62 2.31 62.50 1.50"
                        : "M2.00 83.51 C3.59 82.17 5.13 80.88 6.72 79.63 C9.08 77.74 11.44 75.96 13.80 74.26 C17.75 71.47 21.65 68.87 25.60 66.46 C31.09 63.10 36.63 60.06 42.12 57.24 C49.97 53.23 57.87 49.65 65.72 46.42 C75.93 42.19 86.19 38.49 96.40 35.11 C108.97 30.97 121.59 27.30 134.16 23.98 C149.91 19.81 165.61 16.14 181.36 12.78 C200.24 8.77 219.12 5.23 238.00 2.00"
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
                    path: "M2.00 100.76 C3.59 99.85 5.13 99.00 6.72 98.15 C9.08 96.90 11.44 95.74 13.80 94.61 C17.75 92.73 21.65 91.01 25.60 89.41 C31.09 87.15 36.63 85.11 42.12 83.23 C49.97 80.57 57.87 78.18 65.72 76.02 C75.93 73.23 86.19 70.75 96.40 68.50 C108.97 65.74 121.59 63.29 134.16 61.07 C149.91 58.28 165.61 55.83 181.36 53.60 C200.24 50.94 219.12 48.59 238.00 46.42"
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
                    path: root.standInCompact
                        ? "M1.50 30.50 C1.91 30.39 2.31 30.28 2.72 30.18 C3.33 30.02 3.94 29.87 4.55 29.73 C5.57 29.50 6.58 29.28 7.60 29.08 C9.02 28.80 10.45 28.55 11.87 28.31 C13.90 27.98 15.94 27.68 17.97 27.41 C20.61 27.06 23.26 26.75 25.90 26.46 C29.15 26.12 32.41 25.81 35.66 25.54 C39.73 25.19 43.79 24.89 47.86 24.61 C52.74 24.27 57.62 23.98 62.50 23.70"
                        : "M2.00 118.00 C3.59 117.56 5.13 117.12 6.72 116.71 C9.08 116.09 11.44 115.49 13.80 114.93 C17.75 113.99 21.65 113.11 25.60 112.33 C31.09 111.20 36.63 110.19 42.12 109.25 C49.97 107.90 57.87 106.71 65.72 105.65 C75.93 104.24 86.19 102.98 96.40 101.85 C108.97 100.47 121.59 99.25 134.16 98.15 C149.91 96.78 165.61 95.55 181.36 94.42 C200.24 93.08 219.12 91.92 238.00 90.82"
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
