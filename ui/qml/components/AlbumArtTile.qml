// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Shapes
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components

// The playing track's cover, edge to edge: no inset, radius or frame. A
// track without a cover has a stand-in in its place (with no tile for it,
// a track change between one with a cover and one without moved the whole
// sidebar by the cover's height): the plot's floor colour with one static
// mark at its centre, the same for every track without a cover (a stand-in
// of text repeated the lines under it, and without an album it was blank):
// the app icon's glide and its harmonics, at the icon's own aspect, at a
// cover mark's strength rather than data's. A small tile, as the narrow
// layout's 96 px one, has the mark of three curves, without the 4th
// harmonic. In a light theme a 1 px edge (`standInEdgeColor`) keeps the
// dark tile from reading as a hole in the sidebar. With nothing playing
// (`idle`) it shows only the placeholder colour: the now-playing panel
// shows its compact row then, and while it shrinks into it the tile keeps
// the track that went (a mark of its own there stood alone in an empty
// band).
//
// The tile is a keyboard stop while a track plays: a 2 px ring with a 1 px
// casing inside it shows its focus while the keyboard is in use, over any
// art, and the Menu key or Shift+F10 opens
// its context menu. The pointer over a cover lays a faint veil on it, and
// a double-click opens it in the viewer; the stand-in has neither, as
// there is nothing to open. Assistive technology hears the same: a cover
// is a button that opens the viewer, and the stand-in a button with a menu,
// each saying which keys open its menu, and its press does that (#732).
Rectangle {
    id: root

    required property var uiBridge
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer
    // The tile while nothing plays: the now-playing panel passes the
    // palette's.
    property color placeholderColor: "transparent"
    // Nothing playing: the tile shows no cover, only the placeholder.
    property bool idle: false
    // The stand-in for a track without a cover: its colour, the plot's
    // floor, and the 1 px edge around it, or transparent for none.
    property color standInColor: "transparent"
    property color standInEdgeColor: "transparent"
    // The mark's colours, as drawn on the floor: a cover's mark, not the
    // plot's data. The glide in the icon's amber at the value it had at
    // 68 %, 5.1:1, its hue and chroma kept (mixed toward the floor it read
    // as ochre); the harmonics in its rose and magenta, a little brighter
    // than at 68 %, the 3rd harmonic some 3.8:1 on the floor rather than
    // 3.2, and the 2nd a step lighter between them, 4.4:1, as the icon's
    // rose is, so the two harmonics are not one value.
    property color standInGlideColor: "#b07a1a"
    property color standInHarmonicColor: "#bd5872"
    property color standInHighHarmonicColor: "#b04698"
    // The wide mark's 4th harmonic, as the app icon draws it from 128 px: a
    // faint violet, some 3.2:1 on the floor, the dimmest trace, below the
    // 3rd harmonic's value (at 2.9:1, at 360 px it read as faint as the
    // floor's grain).
    property color standInFourthHarmonicColor: "#8049ac"
    // The mark's strokes, set in pixels so they weigh alike at every size
    // (drawn from one file they were 1.6 to 3.2 px). On the wide mark, a
    // trace drawn with the window's precision: the glide 5 px, 1.3 times
    // the stroke the harmonics taper from, and these 3.4, 2.9 and 2.5 px
    // (at 8.5 px the glide was the loudest amber in the window, heavier
    // than anything else in a window of 1 px lines). On the small mark 4,
    // 3.5 and 3 px (3 and 2.5 px read as hairlines at 152 px), the glide
    // 1.3 times the 4, the focal one, as in the app icon (at the
    // harmonics' taper the curves read as near-equal stripes).
    readonly property real standInStroke: 3.9
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
    // The line inside the ring, which sets it off from the art under it.
    property color focusCasingColor: Kirigami.Theme.backgroundColor
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
    // The ring shows keyboard focus only, as in the lists.
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
        albumArtMenu.popupAt(root, x, y)
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

    // The stand-in's mark and its edge: the app icon's glide and its 2nd,
    // 3rd and faint 4th harmonics (ferrous.svg's own curves, not stretched
    // to 2:1, and the 4th as the icon's 128 and 256 px drawings have it),
    // rising 1 : 2 : 3 : 4 from a common start at the lower left, or in a
    // small tile the glide and its 2nd and 3rd harmonics, as the icon's 32
    // to 64 px drawings have them (the glide and 3rd harmonic alone read as
    // two hairlines). They start a quarter of the way in and, as in the
    // icon, run out to the tile's right edge, cut flush there: a crop of a
    // spectrogram, not a chart's series ending in round caps, nor ending in
    // the tile's free air, as at three quarters of its width. All four
    // leave by that edge on the wide tile, the 4th some 8 % down, and the
    // group about centred in its height (with the 4th out through the top
    // and the others out on the right, at 360 px the crop read as an
    // accident).
    // Drawn as shapes, so their strokes keep the width set for the tile's
    // size (standInStroke) as the curves scale to it. Assistive technology
    // hears "No album art" from the keyboard stop over it.
    component StandInMark: Item {

        Shape {
            id: mark
            objectName: "albumArtStandInMark"
            // The paths' own box, the whole tile, 1000 units a side: the
            // icon's 64-unit drawing at 18.4 units to the icon's one, from a
            // start 240 units in, the ink of its lower three traces 20 units
            // over the tile's middle, the small mark's three too, the 4th
            // harmonic above them, a true one, as in the icon: as far above the
            // 3rd as the 3rd is above the 2nd at every point, flattening as
            // they do (at 16 units it rose 1.25 times its share and bent up
            // at its end). The wide tile draws the four a tenth flatter and
            // lower (placed), so that the 4th too reaches the right edge
            // inside the tile. The paths run on past the tile's right side,
            // and the clip cuts them there.
            readonly property size box: Qt.size(1000, 1000)
            // The stroke the harmonics taper from.
            readonly property real baseStroke: root.standInCompact ? 4 : root.standInStroke
            readonly property real glideStroke: mark.baseStroke * 1.3
            readonly property real harmonicStroke: root.standInCompact ? 3.5 : root.standInStroke * 0.875
            readonly property real highHarmonicStroke: root.standInCompact ? 3 : root.standInStroke * 0.75
            readonly property real fourthHarmonicStroke: mark.baseStroke * 0.645
            // The small tile's paths are the drawing's; the wide one's its
            // points at `rise` of their height, `lift` units down, so the
            // curves stay each other's multiples.
            readonly property real rise: root.standInCompact ? 1 : 0.9
            readonly property real lift: root.standInCompact ? 0 : 146
            function placed(path) {
                return path.replace(/(-?[0-9.]+) (-?[0-9.]+)/g, (point, x, y) =>
                    x + " " + (mark.lift + mark.rise * Number(y)).toFixed(2))
            }
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
                    path: mark.placed("M240.35 487.58 C245.70 478.03 251.05 468.86 256.39 460.01 C264.41 446.75 272.43 434.22 280.45 422.28 C293.82 402.38 307.18 384.13 320.55 367.07 C339.26 343.19 357.97 321.67 376.68 301.77 C400.74 276.19 424.80 253.31 448.86 232.24 C480.93 204.15 513.01 179.30 545.09 156.56 C582.51 130.03 619.94 106.40 657.36 84.69 C705.48 56.78 753.59 32.07 801.71 9.45 C855.17 -15.65 908.63 -38.21 962.10 -58.97 C984.73 -67.77 1007.37 -76.21 1030.00 -84.41")
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
                    path: mark.placed("M240.35 588.97 C245.70 581.81 251.05 574.93 256.39 568.30 C264.41 558.35 272.43 548.95 280.45 540.00 C293.82 525.08 307.18 511.38 320.55 498.59 C339.26 480.68 357.97 464.54 376.68 449.62 C400.74 430.43 424.80 413.27 448.86 397.47 C480.93 376.40 513.01 357.76 545.09 340.71 C582.51 320.81 619.94 303.08 657.36 286.81 C705.48 265.88 753.59 247.34 801.71 230.38 C855.17 211.53 908.63 194.63 962.10 179.06 C984.73 172.47 1007.37 166.12 1030.00 159.98")
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
                    path: mark.placed("M240.35 690.37 C245.70 685.59 251.05 681.00 256.39 676.58 C264.41 669.95 272.43 663.69 280.45 657.72 C293.82 647.77 307.18 638.64 320.55 630.11 C339.26 618.17 357.97 607.41 376.68 597.46 C400.74 584.67 424.80 573.23 448.86 562.70 C480.93 548.65 513.01 536.23 545.09 524.86 C582.51 511.59 619.94 499.77 657.36 488.92 C705.48 474.97 753.59 462.61 801.71 451.30 C855.17 438.74 908.63 427.47 962.10 417.09 C984.73 412.70 1007.37 408.46 1030.00 404.37")
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
                    path: mark.placed("M240.35 791.76 C245.70 789.37 251.05 787.08 256.39 784.87 C264.41 781.55 272.43 778.42 280.45 775.43 C293.82 770.46 307.18 765.90 320.55 761.63 C339.26 755.66 357.97 750.28 376.68 745.31 C400.74 738.91 424.80 733.19 448.86 727.92 C480.93 720.90 513.01 714.69 545.09 709.00 C582.51 702.37 619.94 696.46 657.36 691.04 C705.48 684.06 753.59 677.88 801.71 672.23 C855.17 665.95 908.63 660.31 962.10 655.12 C984.73 652.92 1007.37 650.81 1030.00 648.76")
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
        Accessible.role: root.hasCover ? Accessible.Button : Accessible.ButtonMenu
        Accessible.name: root.hasCover
            ? I18n.i18nc("@info", "Album art")
            : I18n.i18nc("@info", "No album art")
        Accessible.description: root.hasCover
            ? I18n.i18nc("@info read by screen readers, of the playing track's album art; Menu and Shift+F10 are keys",
                         "Opens the album art viewer. Press Menu or Shift+F10 for more actions.")
            : I18n.i18nc("@info read by screen readers, of a track without album art; Menu and Shift+F10 are keys",
                         "Opens a menu of album art actions. Press Menu or Shift+F10 to open it.")
        Accessible.focusable: true
        Accessible.onPressAction: {
            if (root.idle) {
                return
            }
            if (root.hasCover) {
                root.openAlbumArtViewer()
            } else {
                root.openContextMenu(0, root.height)
            }
        }

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

        // Keyboard focus: a 2 px ring inside the tile, which runs edge to
        // edge, and 1 px of casing inside that, so it shows on any art (a
        // 1 px ring alone was 1.2:1 on some).
        Rectangle {
            id: focusRing
            objectName: "albumArtFocusRing"
            anchors.fill: parent
            visible: root.focusRingShown
            color: "transparent"
            border.width: 2
            border.color: root.focusRingColor

            Rectangle {
                objectName: "albumArtFocusCasing"
                anchors.fill: parent
                anchors.margins: focusRing.border.width
                color: "transparent"
                border.width: 1
                border.color: root.focusCasingColor
            }
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

    Components.AppMenu {
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
