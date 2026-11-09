// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components
import "../logic/FormatUtils.js" as FormatUtils

// The title strip of a viewer over the whole screen: a 32 px band over the
// top edge of the view, with a 1 px edge under it, in the colours of the
// plot it sits on (UiPalette's PlotColors), so it reads over the plot or
// the art alike. On the left the playing track's title (semibold),
// "Artist — Album" and "1:17 / 3:49"; on the right flat 24 px tool
// buttons: a zoom readout and Reset while the view is zoomed, the new
// issues while there are any, the viewer's own buttons (declared inside
// the strip, from the left, each taking the strip's `plotColors`) and
// Close. The whole strip shows and hides with the viewer's controls,
// fading as they do; it is drawn over the view, which never resizes for it.
// Turned off in Preferences (`available` false), it never shows: the
// viewer's keys, a double-click and a middle click do what its buttons do.
Rectangle {
    id: root

    required property var uiPalette
    required property var plotColors
    required property var closeViewer
    // The playing track; none without it.
    property var uiBridge: null
    // The playback clock's text of the position, which changes once a second.
    property string positionText: "00:00"
    property bool controlsVisible: true
    // Whether the strip may show at all.
    property bool available: true
    property var pointerActivity: null
    // The view's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    property string resetToolTip: I18n.i18nc("@info:tooltip", "Reset Zoom")
    // The app's errors: new ones show as a button that opens Recent Issues,
    // as the viewer covers the status bar.
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"
    // The viewer's own buttons, before Close.
    default property alias viewerControls: controls.viewerControls
    // Whether keyboard focus is on one of the strip's buttons, which keeps
    // the viewer's controls shown.
    readonly property bool focusInside: {
        for (let item = root.Window.activeFocusItem; item; item = item.parent) {
            if (item === root) {
                return true
            }
        }
        return false
    }

    readonly property var track: root.uiBridge || ({})
    readonly property bool stopped: (root.track.playbackState || "Stopped") === "Stopped"
    // Nothing while stopped, as the window's title.
    readonly property string titleText: {
        if (root.stopped) {
            return ""
        }
        const title = (root.track.currentTrackTitle || "").trim()
        return title.length > 0
            ? title
            : FormatUtils.basenameFromPath((root.track.currentTrackPath || "").trim())
    }
    // Missing values are left out rather than shown as dashes.
    readonly property string artistAlbumText: [
        (root.track.currentTrackArtist || "").trim(),
        (root.track.currentTrackAlbum || "").trim()
    ].filter(function(part) { return part.length > 0 }).join(" — ")
    readonly property string timeText: root.titleText.length === 0
        ? ""
        : I18n.i18nc("@info:status position and length of the playing track", "%1 / %2",
            FormatUtils.clockText(root.positionText),
            FormatUtils.clockText(root.track.durationText))

    implicitHeight: 33
    color: root.plotColors.plotPlate
    opacity: root.available && root.controlsVisible ? 1 : 0
    // Shown at once, so the key that brings the controls back can move
    // focus to them; it fades out, and hidden it takes no pointer.
    visible: root.available && (root.controlsVisible || opacity > 0)

    // Kirigami's durations are 0 with animations turned off.
    Behavior on opacity {
        enabled: Kirigami.Units.shortDuration > 0
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
        }
    }

    Accessible.role: Accessible.ToolBar
    Accessible.name: I18n.i18nc("@label", "Viewer")

    // Clicks on the strip stay on it, rather than reach the view's floor.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
    }

    HoverHandler {
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    // Each text at its own width while they fit; short of room, the time
    // stays whole, the title keeps at least 60% of the rest, and the two
    // others elide.
    Row {
        id: track
        objectName: "viewerStripTrack"
        readonly property real room: Math.max(0, controls.x - 12 - x)
        readonly property real textRoom: Math.max(0, track.room - (timeLabel.visible
            ? timeLabel.implicitWidth + track.spacing : 0) - (contextLabel.visible ? track.spacing : 0))
        readonly property real contextNeed: contextLabel.visible ? contextLabel.implicitWidth : 0
        x: 12
        height: 32
        spacing: 12

        Label {
            id: titleLabel
            objectName: "viewerStripTitle"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth,
                            Math.max(track.textRoom * 0.6, track.textRoom - track.contextNeed))
            text: root.titleText
            visible: text.length > 0
            font.weight: Font.DemiBold
            color: root.plotColors.plotText
            elide: Text.ElideRight
        }

        Label {
            id: contextLabel
            objectName: "viewerStripContext"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, track.textRoom - titleLabel.width))
            text: root.artistAlbumText
            visible: text.length > 0 && root.titleText.length > 0
            color: root.plotColors.plotTextSecondary
            elide: Text.ElideRight
        }

        Label {
            id: timeLabel
            objectName: "viewerStripTime"
            anchors.verticalCenter: parent.verticalCenter
            text: root.timeText
            visible: text.length > 0
            color: root.plotColors.plotTextSecondary
            font.features: root.uiPalette.tabularFigures
        }
    }

    Components.ViewerControls {
        id: controls
        objectName: "viewerStripControls"
        anchors.right: parent.right
        anchors.rightMargin: 4
        y: 4
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        closeViewer: root.closeViewer
        zoomLevel: root.zoomLevel
        resetView: root.resetView
        resetToolTip: root.resetToolTip
        errorController: root.errorController
        closeButtonName: root.closeButtonName
    }

    Rectangle {
        objectName: "viewerStripHairline"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: root.plotColors.plotPlateEdge
    }
}
