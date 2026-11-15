// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

// The playing track's cover, edge to edge: no inset, radius or frame. A
// track without a cover has a stand-in in its place (#373 H11: the panel
// showed no tile for it, so a track change between one with a cover and
// one without moved the whole sidebar by the cover's height): the plot's
// floor colour, with `standInTitle`, the album, in the plot's text colour
// at its top left, `standInInset` in, the inset of the panel's lines,
// wrapping onto up to three lines (#373 H12 Z1: the album and the artist
// at its foot repeated what the lines under it say, and read as a caption
// on a slab). The panel gives it no title where its lines would say the
// same, as without an album: the plain colour then. A tile too small for
// about eight characters of the title a line, as the narrow one with a
// large font, is the plain colour too, as the lines beside it say it. With
// nothing playing (`idle`) it shows neither, only the placeholder colour
// with Breeze's audio disc mark at its centre (#373 H1, H3): the
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
    // floor, its title and the title's colour, the plot's, the font it is
    // sized from, and its inset from the tile's top and sides, which the
    // now-playing panel sets to its lines' inset, so the title starts where
    // they do.
    property color standInColor: "transparent"
    property color standInTextColor: Kirigami.Theme.textColor
    property string standInTitle: ""
    property font standInFont: Kirigami.Theme.defaultFont
    property real standInInset: 2 * Kirigami.Units.largeSpacing
    // The title's size in a tile with room for it, which the panel sets to
    // the palette's display size; in a small tile, the font's own.
    property real standInTitlePointSize: root.standInFont.pointSize * 1.35 * 1.25
    readonly property bool standInShown: !root.idle && root.coverPath.length === 0
    // A tile under ten grid units, such as the narrow layout's 96 px, has
    // its title at the font's size, and where even that leaves under about
    // eight characters a line, none.
    readonly property bool standInCompact: Math.min(root.width, root.height) < 10 * Kirigami.Units.gridUnit
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
    // each {coverPath, from, title}, showing at `from` times
    // `outgoingLevel`, and over the ones after it; `title` is a stand-in's.
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

    // The title of a stand-in, at its top left: up to three lines, fewer
    // where the tile is short, cut off with an ellipsis past that.
    // Assistive technology reads it from the panel's lines.
    component StandInText: Item {
        id: standIn

        property string title: ""
        readonly property real room: Math.max(0, standIn.height - 2 * root.standInInset)
        readonly property real lineWidth: Math.max(0, standIn.width - 2 * root.standInInset)
        // No title where a line has no room for about eight characters.
        readonly property bool fits: standIn.lineWidth >= 8 * titleMetrics.averageCharacterWidth
        readonly property int titleLines: Math.max(1, Math.min(3,
            Math.floor(standIn.room / titleMetrics.lineSpacing)))

        FontMetrics {
            id: titleMetrics
            font: titleText.font
        }

        Text {
            id: titleText
            objectName: "albumArtStandInTitle"
            x: root.standInInset
            y: root.standInInset
            width: standIn.lineWidth
            visible: standIn.fits && text.length > 0
            text: standIn.title
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: standIn.titleLines
            elide: Text.ElideRight
            font.family: root.standInFont.family
            font.pointSize: root.standInCompact
                ? root.standInFont.pointSize : root.standInTitlePointSize
            font.weight: Font.DemiBold
            color: root.standInTextColor
            Accessible.ignored: true
        }
    }

    StandInText {
        objectName: "albumArtStandIn"
        anchors.fill: parent
        visible: root.standInShown
        title: root.standInTitle
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
                // with its texts: without a layer the opacity would take
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

                StandInText {
                    objectName: "albumArtOutgoingStandIn"
                    anchors.fill: parent
                    visible: outgoing.cover !== null && outgoing.coverPath.length === 0
                    title: outgoing.cover !== null ? (outgoing.cover.title || "") : ""
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
