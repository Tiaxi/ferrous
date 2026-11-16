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
// the mark of three curves, without the 4th harmonic. In a light theme a 1 px edge
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
    // the harmonics in its rose and magenta, a little brighter than at
    // 68 %, the 3rd harmonic some 3.8:1 on the floor rather than 3.2 (#373
    // H16 I6), and the 2nd a step lighter between them, 4.4:1, as the
    // icon's rose is (H20: the two harmonics were one value).
    property color standInGlideColor: "#ab7a33"
    property color standInHarmonicColor: "#bd5872"
    property color standInHighHarmonicColor: "#b04698"
    // The wide mark's 4th harmonic, as the app icon draws it from 128 px: a
    // faint violet, some 2.9:1 on the floor, below the 3rd harmonic's value
    // (#373 H21 N1; H24: lifted from 2.6:1 as the icon's was, where its cut
    // on the top edge fused with the rim).
    property color standInFourthHarmonicColor: "#7944a4"
    // The mark's strokes, by the tile's size, so they weigh alike at every
    // size (#373 H16 I6: drawn from one file they were 1.6 to 3.2 px): on
    // the wide mark a stroke of 3.5 px at a 180 px tile, rising to 6.5 px at
    // 360 (H21: H20's 2 and 4 px read faint), the harmonics 0.8, 0.65 and 0.5
    // of it, as the app icon tapers them; on the small mark 4, 3.5 and 3 px
    // (H22: its 3 and 2.5 px read as hairlines at 152 px). The glide 1.3
    // times that stroke, the focal one, as in the app icon (H25: at the
    // harmonics' taper the curves read as near-equal stripes).
    readonly property real standInStroke: Math.min(6.5, 3 + Math.max(0, root.layoutSize - 152) * 3.5 / 208)
    readonly property bool standInShown: !root.idle && root.coverPath.length === 0
    // The tile's size once in place: the now-playing panel keeps it while
    // the tile grows or shrinks, so that the mark is neither drawn again
    // at each size nor changed for the small one on the way.
    property real layoutSize: Math.min(root.width, root.height)
    // A tile under ten grid units, such as the narrow layout's 96 px, has
    // the small mark, of three curves.
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

    // The stand-in's mark and its edge: the app icon's glide and its 2nd,
    // 3rd and faint 4th harmonics (ferrous.svg's own curves, #373 H19: H14
    // to H18 stretched them to 2:1; H21: the 4th, as the icon's 128 and 256
    // px drawings have it), rising 1 : 2 : 3 : 4 from a common start at the
    // lower left, or in a small tile the glide and its 2nd and 3rd
    // harmonics, as the icon's 32 to 64 px drawings have them (#373 H22:
    // H14 F2's glide and 3rd harmonic alone read as two hairlines). They start a quarter of
    // the way in and, as in the icon, run out to the tile's right edge, cut
    // flush there: a crop of a spectrogram, not a chart's series ending in
    // round caps (H19), nor ending in the tile's free air (H20: they ended
    // at three quarters of its width); the 4th leaves through the top edge,
    // as in the icon (H23: it ran out on the right, under the corner).
    // Drawn as shapes, so their strokes keep the width set for the tile's
    // size (standInStroke) as the curves scale to it. Assistive technology
    // hears "No album art" from the keyboard stop over it.
    component StandInMark: Item {

        Shape {
            id: mark
            objectName: "albumArtStandInMark"
            // The paths' own box, the whole tile, 1000 units a side: the
            // icon's 64-unit drawing at 16 units to the icon's one, from a
            // start 240 units in, the ink of its lower three traces centred
            // on the tile's height, the small mark's three too, the 4th
            // harmonic above them (#373 H23: at 12.3 units it ran out under
            // the top right corner, where the icon's leaves through the top
            // edge). The 4th rises 1.25 times its share and bends up over its
            // last stretch, so that it leaves through the top edge some 70 %
            // across at about 40 degrees, as the icon's does (H25: at 1.06
            // times it grazed the edge out at 90 %). The paths run on past the
            // tile's right side, the 4th's past its top, and the clip cuts
            // them there.
            readonly property size box: Qt.size(1000, 1000)
            // The stroke the harmonics taper from.
            readonly property real baseStroke: root.standInCompact ? 4 : root.standInStroke
            readonly property real glideStroke: mark.baseStroke * 1.3
            readonly property real harmonicStroke: root.standInCompact ? 3.5 : root.standInStroke * 0.8
            readonly property real highHarmonicStroke: root.standInCompact ? 3 : root.standInStroke * 0.65
            readonly property real fourthHarmonicStroke: mark.baseStroke * 0.5
            anchors.fill: parent
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
                strokeColor: root.standInCompact ? "transparent" : root.standInFourthHarmonicColor
                strokeWidth: mark.fourthHarmonicStroke
                PathSvg {
                    path: "M240.35 519.94 C244.99 509.59 249.62 499.66 254.25 490.08 C261.20 475.71 268.15 462.13 275.10 449.20 C286.69 427.64 298.27 407.86 309.86 389.39 C326.07 363.52 342.29 340.20 358.51 318.65 C379.36 290.94 400.21 266.14 421.06 243.32 C448.86 212.89 476.66 185.97 504.46 161.33 C520.68 146.96 536.89 133.39 553.11 120.45 C569.33 107.52 585.54 95.23 601.76 83.15 C620.29 69.33 638.83 55.79 657.36 41.89 C671.26 31.47 685.16 20.85 699.06 9.78 C714.86 -2.79 730.66 -15.94 746.46 -30.02"
                }
            }
            ShapePath {
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                scale: mark.pathScale
                strokeColor: root.standInHighHarmonicColor
                strokeWidth: mark.highHarmonicStroke
                PathSvg {
                    path: "M240.35 607.81 C244.99 601.60 249.62 595.64 254.25 589.90 C261.20 581.27 268.15 573.13 275.10 565.37 C286.69 552.43 298.27 540.57 309.86 529.48 C326.07 513.96 342.29 499.97 358.51 487.04 C379.36 470.41 400.21 455.53 421.06 441.84 C448.86 423.58 476.66 407.43 504.46 392.65 C536.89 375.41 569.33 360.04 601.76 345.93 C643.46 327.79 685.16 311.73 726.86 297.03 C773.20 280.70 819.53 266.05 865.86 252.55 C920.58 236.62 975.29 222.29 1030.00 209.07"
                }
            }
            ShapePath {
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                scale: mark.pathScale
                strokeColor: root.standInHarmonicColor
                strokeWidth: mark.harmonicStroke
                PathSvg {
                    path: "M240.35 695.69 C244.99 691.55 249.62 687.57 254.25 683.74 C261.20 677.99 268.15 672.56 275.10 667.39 C286.69 658.77 298.27 650.86 309.86 643.47 C326.07 633.12 342.29 623.79 358.51 615.17 C379.36 604.09 400.21 594.17 421.06 585.04 C448.86 572.87 476.66 562.10 504.46 552.25 C536.89 540.75 569.33 530.51 601.76 521.10 C643.46 509.01 685.16 498.30 726.86 488.50 C773.20 477.61 819.53 467.85 865.86 458.85 C920.58 448.23 975.29 438.68 1030.00 429.86"
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
                    path: "M240.35 783.56 C244.99 781.49 249.62 779.51 254.25 777.59 C261.20 774.72 268.15 772.00 275.10 769.41 C286.69 765.10 298.27 761.15 309.86 757.45 C326.07 752.28 342.29 747.61 358.51 743.30 C379.36 737.76 400.21 732.80 421.06 728.24 C448.86 722.15 476.66 716.77 504.46 711.84 C536.89 706.09 569.33 700.97 601.76 696.27 C643.46 690.22 685.16 684.87 726.86 679.97 C773.20 674.52 819.53 669.64 865.86 665.14 C920.58 659.83 975.29 655.06 1030.00 650.65"
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
