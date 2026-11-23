// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// A plate on a visualization, in the plot's own colours (UiPalette's
// PlotColors) with a 1 px edge outside it. With `text`, it is a lane label;
// otherwise it holds the items declared inside it, such as PlotToolButtons,
// 2 px apart with 2 px around them, or `contentSpacing` apart with
// `padding` around them. A plate that is not `revealed` fades
// out but keeps its place and its Tab stops, so the keyboard can reveal it.
// A `compact` label, for a short lane, is 16 px tall with 11 px text, both
// larger with a larger font, rather than 20 px with 12 px text. A larger
// font's label is its capitals and the room around them tall (from the
// text's line, a letter at 24 pt stood on a plate 28 by 48 px), and never
// shorter than the text's line, which its box holds (over the whole screen
// at 24 pt, the capitals' room was 3 px short of it).
Item {
    id: root

    required property var plotColors
    required property var uiPalette
    property string text: ""
    readonly property bool isLabel: root.text.length > 0
    property bool compact: false
    // How much larger than the window's its label's text is: the type scale
    // of a viewer over the whole screen (UiPalette.fullscreenTextScale),
    // whose plot's text is on its strip's scale; 1 elsewhere.
    property real textScale: 1
    readonly property real labelPointSize: (root.compact ? root.uiPalette.compactPointSize
                                                          : root.uiPalette.smallPointSize) * root.textScale
    // The smallest the plate is either way: a label's height, and the
    // narrowest it is.
    readonly property real minimumSize: root.compact ? 16 : 20
    // A label's height, from the font alone.
    readonly property real labelHeight: Math.max(root.minimumSize, Math.ceil(capitalMetrics.tightBoundingRect.height)
        + 2 * Math.round((root.compact ? 3 : 4) * root.uiPalette.fontScale), Math.ceil(lineMetrics.height))
    property real padding: 2
    property real horizontalPadding: root.isLabel ? (root.compact ? 4 : 6) : root.padding
    property real verticalPadding: root.isLabel ? 0 : root.padding
    property real contentSpacing: 2
    property real radius: root.isLabel ? 3 : 4
    // Whether presses, clicks and double-clicks on the plate, between its
    // buttons too, stay on it rather than reach the view under it.
    property bool takesPointer: false
    // Without a fill and an edge of its own, on a plate it shares with
    // other controls, such as the windowed viewer's corner.
    property bool bare: false
    property bool revealed: true
    // A level the plate fades with besides its own reveal, such as a
    // viewer's controls as they hide: from 1 to 0.
    property real fadeLevel: 1
    default property alias contentData: content.data
    // A cell at the plate's start for what lies over it, such as a lane's
    // label, set off by a 1 px divider, 6 px in from the plate's edges as
    // the viewers' are: the plate's items come after it. It is none of
    // theirs, so it takes no pointer and no hover from the view under it.
    property real leadingCell: 0
    // How much of the plate's own fill and edge shows under its content:
    // none of a label's on a plate it shares (PlotLaneHead).
    property real backgroundLevel: 1
    // How far the plate is revealed, from 0 to 1, as it fades in or out.
    property real revealLevel: root.revealed ? 1 : 0

    implicitWidth: Math.max(root.minimumSize, content.implicitWidth + 2 * root.horizontalPadding)
    implicitHeight: root.isLabel ? root.labelHeight
        : Math.max(root.minimumSize, content.implicitHeight + 2 * root.verticalPadding)

    opacity: root.revealLevel * root.fadeLevel

    // Kirigami's durations are 0 with animations turned off. While the
    // plate fades with `fadeLevel`, such as a viewer's controls coming back
    // with the pointer on the view, its own reveal follows at once: the
    // plate, its fill and its glyphs, come in on that one level rather than
    // on two curves, one after the other.
    Behavior on revealLevel {
        enabled: Kirigami.Units.shortDuration > 0 && root.fadeLevel >= 1
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
        }
    }

    Accessible.role: root.isLabel ? Accessible.StaticText : Accessible.Grouping
    Accessible.name: root.text

    // Outside the plate: a border drawn inside a rectangle 1 px larger.
    Rectangle {
        objectName: "plotPlateEdge"
        visible: !root.bare
        opacity: root.backgroundLevel
        x: -1
        y: -1
        width: parent.width + 2
        height: parent.height + 2
        radius: root.radius + 1
        color: "transparent"
        border.width: 1
        border.color: root.plotColors.plotPlateEdge
    }

    Rectangle {
        objectName: "plotPlateFill"
        visible: !root.bare
        opacity: root.backgroundLevel
        anchors.fill: parent
        radius: root.radius
        color: root.plotColors.plotPlate
    }

    Rectangle {
        objectName: "plotPlateDivider"
        visible: root.leadingCell > 0
        x: root.leadingCell
        y: 6
        width: 1
        height: Math.max(0, parent.height - 2 * y)
        color: root.plotColors.plotPlateEdge
    }

    MouseArea {
        objectName: "plotPlatePointerArea"
        anchors.fill: parent
        anchors.margins: -1
        enabled: root.takesPointer
        visible: root.takesPointer
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        // Taken, so a double-click on the plate never reaches the view's.
        onDoubleClicked: function(mouse) { mouse.accepted = true }
    }

    TextMetrics {
        id: capitalMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.labelPointSize
        font.weight: Font.DemiBold
        text: "E"
    }

    FontMetrics {
        id: lineMetrics
        font: capitalMetrics.font
    }

    Row {
        id: content
        anchors.centerIn: parent
        // After a leading cell and its divider, to the pixel: centring
        // rounds the plate's half up, so the cell's is rounded down.
        anchors.horizontalCenterOffset: root.leadingCell > 0 ? Math.floor((root.leadingCell + 1) / 2) : 0
        spacing: root.contentSpacing

        // Text, not a Label: the desktop style's Label has a hover handler,
        // which would take the hover from the view under the plate. Its
        // letters are drawn as curves, with grey antialiasing (the
        // platform's subpixel antialiasing fringed them in colour on the
        // plate, which is not the window's).
        Text {
            objectName: "plotPlateLabel"
            visible: root.isLabel
            renderType: Text.CurveRendering
            textFormat: Text.PlainText
            text: root.text
            font.family: root.uiPalette.baseFont.family
            font.pointSize: root.labelPointSize
            font.weight: Font.DemiBold
            color: root.plotColors.plotText
            height: root.labelHeight
            verticalAlignment: Text.AlignVCenter
            Accessible.ignored: true
        }
    }
}
