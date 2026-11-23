// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// The visualization viewer's controls where it has no title strip: in the
// window, and over the whole screen with the strip turned off. They are the
// strip's own (ViewerControls), in the plot's colours on one plate in the
// top right corner: [Not following | Zoom ↺] | [⚠] [modes] | [✕], the modes declared inside
// it as they are inside the strip. So the viewer has one toolbar everywhere
// (the corner's buttons were the plot's, with another checked fill, glyph
// ink, warning colour and order than the strip's), and it ends in Close
// there too, over the whole screen with the strip turned off. The plate is
// as tall as the plot's, with 4 px around the buttons, so its top and
// centre line are the lane heads'.
Item {
    id: root

    required property var plotColors
    required property var uiPalette
    required property var closeViewer
    property bool closeShown: true
    property bool controlsVisible: true
    // The controls as they fade (FullscreenControlsAutoHide's level); at
    // once without one.
    property real controlsLevel: root.controlsVisible ? 1 : 0
    // Whether the plate may show at all.
    property bool available: true
    property var pointerActivity: null
    // The plot's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    // Whether the view does not follow the playhead, and Follow Playhead's
    // shortcut; see FollowNotice.
    property bool noticeShown: false
    property string followShortcut: ""
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"
    // The viewer's own buttons, before Close.
    default property alias viewerControls: controls.viewerControls

    readonly property real padding: 4

    implicitWidth: controls.implicitWidth + 2 * root.padding
    implicitHeight: controls.implicitHeight + 2 * root.padding
    // As the title strip of the viewer over the whole screen: shown at once,
    // so the key that brings the controls back can move focus to them; it
    // fades out on the strip's curve, and hidden it takes no pointer.
    opacity: root.controlsLevel
    visible: root.available && (root.controlsVisible || root.controlsLevel > 0)

    Accessible.role: Accessible.Grouping
    Accessible.name: I18n.i18nc("@label", "Viewer")

    // Whether the pointer is on the plate, which counts as on the view.
    readonly property bool hovered: root.visible && cornerHover.hovered

    HoverHandler {
        id: cornerHover
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    // The plate, under the controls; presses between them stay on it.
    Components.PlotPlate {
        objectName: "viewerCornerPlate"
        anchors.fill: parent
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        takesPointer: true
        Accessible.ignored: true
    }

    Components.ViewerControls {
        id: controls
        objectName: "viewerCornerButtons"
        x: root.padding
        y: root.padding
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        platePadding: root.padding
        closeViewer: root.closeViewer
        closeShown: root.closeShown
        closeButtonName: root.closeButtonName
        zoomLevel: root.zoomLevel
        resetView: root.resetView
        noticeShown: root.noticeShown
        followShortcut: root.followShortcut
        errorController: root.errorController
        dividers: true
        closeDivided: true
    }
}
