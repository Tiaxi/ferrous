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
    // faint violet, some 2.6:1 on the floor, below the 3rd harmonic's value
    // (#373 H21 N1).
    property color standInFourthHarmonicColor: "#723b9f"
    // The mark's strokes, by the tile's size, so they weigh alike at every
    // size (#373 H16 I6: drawn from one file they were 1.6 to 3.2 px): on
    // the wide mark the glide's 3.5 px at a 180 px tile, rising to 6.5 px at
    // 360 (H21: H20's 2 and 4 px read faint), and the harmonics 0.8, 0.65
    // and 0.5 of it, as the app icon tapers them; on the small mark 4, 3.5
    // and 3 px (H22: its 3 and 2.5 px read as hairlines at 152 px).
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
    // at three quarters of its width).
    // Drawn as shapes, so their strokes keep the width set for the tile's
    // size (standInStroke) as the curves scale to it. Assistive technology
    // hears "No album art" from the keyboard stop over it.
    component StandInMark: Item {

        Shape {
            id: mark
            objectName: "albumArtStandInMark"
            // The paths' own box, the whole tile, 1000 units a side: the
            // icon's 64-unit drawing at 12.3 units to the icon's one, from a
            // start 240 units in, the ink of its lower three traces centred
            // on the tile's height, the small mark's three too, the 4th
            // harmonic above them. The paths run on past
            // the tile's right side, and the clip cuts them there.
            readonly property size box: Qt.size(1000, 1000)
            readonly property real glideStroke: root.standInCompact ? 4 : root.standInStroke
            readonly property real harmonicStroke: root.standInCompact ? 3.5 : root.standInStroke * 0.8
            readonly property real highHarmonicStroke: root.standInCompact ? 3 : root.standInStroke * 0.65
            readonly property real fourthHarmonicStroke: mark.glideStroke * 0.5
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
                    path: "M240.35 534.90 C243.92 528.53 247.48 522.42 251.05 516.52 C256.39 507.68 261.74 499.32 267.09 491.36 C276.00 478.10 284.91 465.93 293.82 454.56 C306.29 438.64 318.77 424.29 331.24 411.03 C347.28 393.97 363.32 378.71 379.36 364.67 C400.74 345.94 422.13 329.38 443.51 314.22 C468.46 296.53 493.41 280.77 518.36 266.30 C550.44 247.70 582.51 231.23 614.59 216.15 C650.23 199.39 685.87 184.37 721.52 170.53 C766.07 153.23 810.62 137.77 855.17 123.56 C913.45 104.97 971.72 88.51 1030.00 73.44"
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
                    path: "M240.35 602.49 C243.92 597.72 247.48 593.13 251.05 588.71 C256.39 582.08 261.74 575.81 267.09 569.84 C276.00 559.89 284.91 550.77 293.82 542.24 C306.29 530.30 318.77 519.54 331.24 509.59 C347.28 496.80 363.32 485.36 379.36 474.82 C400.74 460.78 422.13 448.35 443.51 436.98 C468.46 423.72 493.41 411.90 518.36 401.05 C550.44 387.09 582.51 374.74 614.59 363.43 C650.23 350.86 685.87 339.60 721.52 329.22 C766.07 316.24 810.62 304.65 855.17 293.99 C913.45 280.05 971.72 267.70 1030.00 256.40"
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
                    path: "M240.35 670.09 C243.92 666.90 247.48 663.85 251.05 660.90 C256.39 656.48 261.74 652.30 267.09 648.32 C276.00 641.69 284.91 635.60 293.82 629.92 C306.29 621.96 318.77 614.79 331.24 608.15 C347.28 599.63 363.32 592.00 379.36 584.97 C400.74 575.61 422.13 567.33 443.51 559.75 C468.46 550.91 493.41 543.03 518.36 535.79 C550.44 526.49 582.51 518.25 614.59 510.71 C650.23 502.34 685.87 494.83 721.52 487.90 C766.07 479.25 810.62 471.53 855.17 464.42 C913.45 455.13 971.72 446.90 1030.00 439.36"
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
                    path: "M240.35 737.68 C243.92 736.09 247.48 734.56 251.05 733.09 C256.39 730.88 261.74 728.79 267.09 726.80 C276.00 723.48 284.91 720.44 293.82 717.60 C306.29 713.62 318.77 710.03 331.24 706.72 C347.28 702.45 363.32 698.64 379.36 695.13 C400.74 690.45 422.13 686.30 443.51 682.51 C468.46 678.09 493.41 674.15 518.36 670.54 C550.44 665.88 582.51 661.77 614.59 658.00 C650.23 653.81 685.87 650.05 721.52 646.59 C766.07 642.27 810.62 638.40 855.17 634.85 C913.45 630.20 971.72 626.09 1030.00 622.32"
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
