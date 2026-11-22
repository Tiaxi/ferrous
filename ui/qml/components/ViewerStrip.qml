// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components
import "../logic/FormatUtils.js" as FormatUtils

// The title strip of a viewer over the whole screen: a 38 px band over the
// top edge of the view (taller with a larger font, by its line and 4 px
// above and below it), with a 1 px edge under it, in the colours of the
// plot it sits on (UiPalette's PlotColors), so it reads over the plot or
// the art alike. Shown, it is opaque (plotStrip): the plot, its playhead
// and its crosshair end at its lower edge rather than show through. On the
// left the playing track's title (semibold), "Artist — Album" and
// "1:17 / 3:49", on the type scale of the viewer's text over the whole
// screen (UiPalette.fullscreenTextScale), as the album art's details under
// it are (at the window's size they were a second scale); on the
// right, 8 px in from the edge, flat 24 px tool buttons: a zoom readout and
// Reset's glyph while the view is zoomed, the new issues while there are any, the
// viewer's own buttons (declared inside the strip, from the left, each taking
// the strip's `plotColors`) and Close: the album art's info toggle, or the
// visualization's mode switch, which with its zoom is here rather
// than in the plot's corner while the strip is on, so the strip is the viewer's one
// place for its controls (a plate of them under the strip's Close read as
// a second cluster). The whole strip shows and hides with the viewer's
// controls, fading as they do; it is drawn over the view, which never
// resizes for it. Turned off in Preferences (`available` false), it never
// shows: the viewer's keys and a double-click do what its buttons do.
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
    // The controls as they fade (FullscreenControlsAutoHide's level); at
    // once without one.
    property real controlsLevel: root.controlsVisible ? 1 : 0
    // Whether the strip may show at all.
    property bool available: true
    // How much of the track's text shows besides the strip's own fade:
    // less where what is under the strip at rest takes its place, as the
    // visualization's first lane's label does (PlotLanes.stripTextLevel).
    property real trackLevel: 1
    property var pointerActivity: null
    // The view's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    // Whether the view does not follow the playhead, and Follow Playhead's
    // shortcut; see FollowNotice.
    property bool noticeShown: false
    property string followShortcut: ""
    property string resetToolTip: I18n.i18nc("@info:tooltip", "Reset Zoom")
    // The app's errors: new ones show as a button that opens Recent Issues,
    // as the viewer covers the status bar.
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"
    // The zoom, and the viewer's own buttons, each set off from what follows
    // by a 1 px divider (ViewerControls' `dividers` and `closeDivided`).
    property bool dividers: false
    property bool closeDivided: false
    // The viewer's own buttons, before Close.
    default property alias viewerControls: controls.viewerControls
    // Whether the pointer is on the strip, which counts as on the view.
    readonly property bool hovered: stripHover.hovered
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

    // How far in from the strip's right edge Close's centre is.
    readonly property real closeAxisInset: controls.anchors.rightMargin + root.uiPalette.toolButtonSize / 2
    // Its text's size, as a factor of the window's.
    readonly property real textScale: root.uiPalette.fullscreenTextScale
    readonly property real textPointSize: root.uiPalette.basePointSize * root.textScale
    // The band over the edge: 32 px as the text grows, 38 px, or the line
    // of its text or its buttons and 4 px above and below them, whichever
    // is taller.
    readonly property real bandHeight: Math.max(Math.round(32 * root.textScale),
        Math.ceil(Math.max(titleLabel.implicitHeight, contextLabel.implicitHeight, timeLabel.implicitHeight)) + 8,
        controls.implicitHeight + 8)
    implicitHeight: root.bandHeight + 1
    color: root.plotColors.plotStrip
    opacity: root.available ? root.controlsLevel : 0
    // Shown at once, so the key that brings the controls back can move
    // focus to them; it fades out, and hidden it takes no pointer.
    visible: root.available && (root.controlsVisible || root.controlsLevel > 0)

    Accessible.role: Accessible.ToolBar
    Accessible.name: I18n.i18nc("@label", "Viewer")

    // Clicks on the strip stay on it, rather than reach the view's floor.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
    }

    HoverHandler {
        id: stripHover
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    // Each text at its own width while they fit; short of room, the time
    // stays whole, the title keeps at least 60% of the rest, and the two
    // others elide. Beside "Not following", which can leave the time too
    // little room on a narrow screen with a large font, it is left out
    // rather than run under the notice.
    Row {
        id: track
        objectName: "viewerStripTrack"
        opacity: root.trackLevel
        readonly property real room: Math.max(0, controls.x - track.spacing - x)
        readonly property real textRoom: Math.max(0, track.room - (timeLabel.visible
            ? timeLabel.implicitWidth + track.spacing : 0) - (contextLabel.visible ? track.spacing : 0))
        readonly property real contextNeed: contextLabel.visible ? contextLabel.implicitWidth : 0
        // 16 px in from the edge (at 12 px, the gap between its own texts,
        // the title sat too near the edge).
        x: 2 * Kirigami.Units.largeSpacing
        height: root.bandHeight
        // 14 px at the default font, and as much more as the font is larger
        // (at 24 pt, 14 px beside 38 px type ran the album into the time).
        spacing: root.uiPalette.scaledSize(Math.round(12 * root.textScale))

        Label {
            id: titleLabel
            objectName: "viewerStripTitle"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth,
                            Math.max(track.textRoom * 0.6, track.textRoom - track.contextNeed))
            textFormat: Text.PlainText
            text: root.titleText
            visible: text.length > 0
            font.pointSize: root.textPointSize
            font.weight: Font.DemiBold
            color: root.plotColors.plotText
            elide: Text.ElideRight
        }

        Label {
            id: contextLabel
            objectName: "viewerStripContext"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, track.textRoom - titleLabel.width))
            textFormat: Text.PlainText
            text: root.artistAlbumText
            visible: text.length > 0 && root.titleText.length > 0
            font.pointSize: root.textPointSize
            color: root.plotColors.plotTextSecondary
            elide: Text.ElideRight
        }

        Label {
            id: timeLabel
            objectName: "viewerStripTime"
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: root.timeText
            visible: text.length > 0 && (!root.noticeShown || timeLabel.implicitWidth <= track.room)
            color: root.plotColors.plotTextSecondary
            font.pointSize: root.textPointSize
            font.features: root.uiPalette.tabularFigures
        }
    }

    Components.ViewerControls {
        id: controls
        objectName: "viewerStripControls"
        anchors.right: parent.right
        // 8 px in from the screen's edge (at 4 px, Close's focus ring met
        // the screen's corner).
        anchors.rightMargin: Kirigami.Units.largeSpacing
        y: Math.round((root.bandHeight - height) / 2)
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        closeViewer: root.closeViewer
        zoomLevel: root.zoomLevel
        // On the strip's one type scale, as the time is (at the window's
        // small size it was 12 px in a 16 px strip).
        readoutPointSize: root.textPointSize
        resetView: root.resetView
        noticeShown: root.noticeShown
        followShortcut: root.followShortcut
        resetToolTip: root.resetToolTip
        errorController: root.errorController
        closeButtonName: root.closeButtonName
        dividers: root.dividers
        closeDivided: root.closeDivided
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
