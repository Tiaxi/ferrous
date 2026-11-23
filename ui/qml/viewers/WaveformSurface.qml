// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ChannelLabels.js" as ChannelLabels
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/PlotLanes.js" as PlotLanes

Item {
    id: root

    required property var uiBridge
    required property var uiPalette
    // Where playback is; see PlaybackClock.
    property var clock: null
    property var seekCommitted: null
    property bool interactiveOverlaysVisible: true
    // The overlays as they fade with the viewer's controls, from 1 to 0:
    // the crosshair shows until they have faded out. Without a level of
    // the viewer's, they show and hide at once.
    property real interactiveOverlaysLevel: root.interactiveOverlaysVisible ? 1 : 0
    // How much of the crosshair shows besides that: none while the pointer
    // is on a plate of the plot (PlotLayer's crosshairLevel).
    property real crosshairLevel: 1
    // Whether the plot draws the playhead; see Main.qml's mode switch.
    property bool playheadVisible: true
    // Whether a viewer shows it, where its lanes' seam is the viewer's.
    property bool viewerMode: false
    property var pointerActivity: null
    property size reservedCorner: Qt.size(0, 0)
    // What the lowest lane's label and buttons take of it; see PlotLayer.
    property rect laneHeadBounds: Qt.rect(0, 0, 0, 0)
    // The plot's width: the level meter's column takes the rest.
    property real plotWidth: root.width
    // How much larger than the window's the crosshair's readout is: as the
    // lane heads' (PlotLayer.textScale).
    property real textScale: 1
    // A time of the track to follow, such as where its file is cut off, or
    // -1; markerX is where the plot draws it, NaN while it does not.
    property real markerSeconds: -1
    readonly property real markerX: waveform.markerX
    readonly property alias waveformItem: waveform
    // The pointer hides once the overlays have faded out, and stays so.
    // While a pan holds the plot it is a closed hand, as over the album
    // art's.
    readonly property int pointerCursorShape: !root.interactiveOverlaysVisible && root.interactiveOverlaysLevel <= 0
        ? Qt.BlankCursor
        : waveform.panning ? Qt.ClosedHandCursor : Qt.ArrowCursor
    readonly property var plotColors: root.uiPalette.waveformPlot
    // The lanes: one per channel in the per-channel view, one otherwise.
    readonly property int laneCount: waveform.viewMode === 1 ? Math.max(1, waveform.channelCount) : 1
    // Before any audio, with neither a track nor its channels, the pane is
    // at rest, as the spectrogram's: per channel, the lanes of
    // a stereo track, named, each with its centre line; the plot layer
    // gives them no Mute and Solo then. A track change keeps the lanes
    // while the new track's channels are not known yet.
    readonly property bool atRest: (root.uiBridge.currentTrackPath || "").length === 0
        && !(root.uiBridge.currentTrackChannels > 0)
    readonly property var laneLabels: {
        const labels = []
        const named = waveform.viewMode === 1
        for (let i = 0; i < root.laneCount; ++i) {
            labels.push(named ? root.channelLabel(i) : "")
        }
        return labels
    }
    // The lane the pointer is on, or -1.
    readonly property int hoveredLane: waveformHover.hovered
        ? PlotLanes.laneAt(waveformHover.point.position.y, root.laneCount, root.height)
        : -1
    // Kept for the tests and the per-channel lanes: the channel row the
    // pointer is on, or -1.
    readonly property int hoveredChannel: waveform.viewMode === 1 ? root.hoveredLane : -1
    // Where the pointer is on the plot, in the scene, while it is on it.
    readonly property point pointerScenePosition: waveformHover.point.scenePosition
    // Whether the plot waits for the track's overview.
    readonly property bool awaitingData: !root.uiBridge.waveformComplete
        && root.uiBridge.waveformCoverageSeconds <= 0
    // Whether the plot has drawn the track's waveform, or has none to wait
    // for (WaveformEditorItem's pictureReady): false from when it is shown
    // until its first frame that draws the track's detail of the view.
    readonly property bool pictureReady: waveform.pictureReady

    Accessible.role: Accessible.Graphic
    Accessible.name: I18n.i18nc("@label", "Waveform")
    Accessible.description: I18n.i18nc("@info", "Amplitude of the playing track over time")

    function channelLabel(index) {
        return ChannelLabels.label(waveform.channelCount, index,
                                   root.uiBridge.currentTrackChannelLabels || [])
    }

    Rectangle {
        objectName: "waveformFloor"
        anchors.fill: parent
        color: root.plotColors.plotBg
    }

    WaveformEditorItem {
        id: waveform
        objectName: "waveformEditorItem"
        width: root.plotWidth
        height: root.height
        // Hidden too: the item decodes nothing while it is not shown, and
        // keeps the detail it has, so that the mode switch back draws it at
        // once. A new track while hidden clears it.
        sourcePath: root.uiBridge.currentTrackPath
        overviewData: root.uiBridge.waveformPeaksPacked
        overviewCoverageSeconds: root.uiBridge.waveformCoverageSeconds
        overviewComplete: root.uiBridge.waveformComplete
        clock: root.clock
        durationSeconds: root.uiBridge.durationSeconds
        playing: (root.uiBridge.playbackState || "") === "Playing"
        zoomEnabled: root.uiBridge.spectrogramZoomEnabled
        // The scale is the spectrogram's: on the waveform, time and level
        // labels collide with the data.
        gridEnabled: false
        crosshairEnabled: root.uiBridge.showSpectrogramCrosshair
            && (root.interactiveOverlaysVisible || root.interactiveOverlaysLevel > 0)
        crosshairOpacity: root.interactiveOverlaysLevel * root.crosshairLevel
        showFpsOverlay: root.uiBridge.showFps
        reservedCorner: root.reservedCorner
        laneHeadBounds: root.laneHeadBounds
        viewMode: root.uiBridge.spectrogramViewMode
        channelCountHint: root.atRest ? 2 : Math.max(1, root.uiBridge.currentTrackChannels)
        markerSeconds: root.markerSeconds
        // Per channel: a 64-bit mask loses its low bits as a QML number.
        mutedChannels: root.uiBridge.mutedChannels || []
        soloedChannels: root.uiBridge.soloedChannels || []
        // The waveform plot set: on the view colour, in the pane's waveform
        // colour, the seek bar's in a light theme and quieter in a dark one.
        backgroundColor: root.plotColors.plotBg
        waveformColor: root.uiPalette.paneWave
        mutedWaveformColor: ColorUtils.mixColor(root.uiPalette.paneWave, root.plotColors.plotBg, 0.65)
        // Before any audio each lane is a silent one: its centre line, and
        // the seam between lanes the stronger line.
        centerLineColor: root.atRest ? root.uiPalette.waveformRestLine
                                     : ColorUtils.withAlpha(root.uiPalette.text, 0.08)
        laneSeamColor: root.atRest ? root.uiPalette.seam
            : root.viewerMode ? root.uiPalette.viewerWaveformLaneSeam : root.plotColors.plotLaneSeam
        // The crosshair's readout on the plot's plates, as the
        // spectrogram's, and its dashes in the plot's text.
        readoutPlateColor: root.plotColors.plotPlate
        readoutPlateEdgeColor: root.plotColors.plotPlateEdge
        readoutTextColor: root.plotColors.plotText
        textScale: root.textScale
        // Dark: a light core in a black casing; light: a dark core in white.
        playheadColor: root.uiPalette.themeIsDark ? "#e8e8e8" : "#202326"
        playheadCasingColor: root.uiPalette.themeIsDark
            ? Qt.rgba(0, 0, 0, 0.6) : Qt.rgba(1, 1, 1, 0.85)
        playheadVisible: root.playheadVisible
        onSeekRequested: function(seconds) {
            if (root.seekCommitted) {
                root.seekCommitted(seconds)
            }
        }

        // Before any audio, the seam between lanes is 3 px, centred on its
        // own, as the spectrogram's groove is, so that it is the stronger
        // line and the pane reads as silent channels (with lines at half
        // of full scale in each lane, stronger than the seam and the centre
        // line, two lanes read as eight empty rows of a table). Gone with
        // the first track.
        Repeater {
            model: root.atRest ? Math.max(0, root.laneCount - 1) : 0

            delegate: Rectangle {
                required property int index
                objectName: "waveformRestSeam"
                y: Math.round((index + 1) * waveform.height / root.laneCount) - 1
                width: waveform.width
                height: 3
                color: root.uiPalette.seam
            }
        }

        // The only hover handler over the waveform, and on the item itself.
        // The item takes hover for its crosshair, so a handler on a parent
        // never hears of the pointer. A hovered handler on an item above,
        // even one that lets hover through, keeps it from the item instead.
        HoverHandler {
            id: waveformHover
            objectName: "waveformSurfaceHoverHandler"
            cursorShape: root.pointerCursorShape
            // A touch ends without a hover leave for the item.
            onHoveredChanged: {
                if (!hovered) {
                    waveform.setHoverPosition(0, 0, false)
                }
            }
            onPointChanged: {
                if (root.pointerActivity) {
                    root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
                }
            }
        }
    }
}
