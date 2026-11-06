// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import FerrousUi 1.0
import "." as Components
import "../logic/FormatUtils.js" as FormatUtils

// The title strip of a viewer over the whole screen: 32 px of chrome with a
// hairline under it. On the left the playing track's title (semibold),
// "Artist — Album" and "1:17 / 3:49"; on the right flat 24 px tool
// buttons: a zoom readout and Reset while the view is zoomed, the new
// issues while there are any, the viewer's own buttons (declared inside
// the strip) and Close. The buttons follow the auto-hide of the viewer's
// controls; the strip itself stays, so the view under it never resizes.
Rectangle {
    id: root

    required property var uiPalette
    required property var closeViewer
    // The playing track; none without it.
    property var uiBridge: null
    // The playback clock's text of the position, which changes once a second.
    property string positionText: "00:00"
    property bool controlsVisible: true
    property var pointerActivity: null
    // The view's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    property string resetToolTip: I18n.i18nc("@info:tooltip", "Reset Visualization View")
    // The app's errors: new ones show as a button that opens Recent Issues,
    // as the viewer covers the status bar.
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"
    // The viewer's own buttons, before Close.
    default property alias viewerControls: controls.viewerControls

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
    color: root.uiPalette.chrome

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

    RowLayout {
        objectName: "viewerStripTrack"
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.right: controls.left
        anchors.rightMargin: 12
        anchors.top: parent.top
        height: 32
        spacing: 12

        Label {
            objectName: "viewerStripTitle"
            text: root.titleText
            visible: text.length > 0
            font.weight: Font.DemiBold
            color: root.uiPalette.text
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
            Layout.alignment: Qt.AlignVCenter
        }

        Label {
            objectName: "viewerStripContext"
            text: root.artistAlbumText
            visible: text.length > 0 && root.titleText.length > 0
            color: root.uiPalette.textSecondary
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
            Layout.alignment: Qt.AlignVCenter
        }

        Label {
            objectName: "viewerStripTime"
            text: root.timeText
            visible: text.length > 0
            color: root.uiPalette.textSecondary
            font.features: root.uiPalette.tabularFigures
            Layout.alignment: Qt.AlignVCenter
        }

        Item {
            Layout.fillWidth: true
        }
    }

    Components.ViewerControls {
        id: controls
        objectName: "viewerStripControls"
        anchors.right: parent.right
        anchors.rightMargin: 4
        y: 4
        uiPalette: root.uiPalette
        closeViewer: root.closeViewer
        controlsVisible: root.controlsVisible
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
        color: root.uiPalette.hairline
    }
}
