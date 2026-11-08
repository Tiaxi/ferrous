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
    property var pointerActivity: null
    property size reservedCorner: Qt.size(0, 0)
    // The plot's width: the level meter's column takes the rest.
    property real plotWidth: root.width
    // A time of the track to follow, such as where its file is cut off, or
    // -1; markerX is where the plot draws it, NaN while it does not.
    property real markerSeconds: -1
    readonly property real markerX: waveform.markerX
    readonly property alias waveformItem: waveform
    readonly property var plotColors: root.uiPalette.waveformPlot
    // The lanes: one per channel in the per-channel view, one otherwise.
    readonly property int laneCount: waveform.viewMode === 1 ? Math.max(1, waveform.channelCount) : 1
    readonly property var laneLabels: {
        const labels = []
        for (let i = 0; i < root.laneCount; ++i) {
            labels.push(waveform.viewMode === 1 ? root.channelLabel(i) : "")
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
    // Whether the plot waits for the track's overview.
    readonly property bool awaitingData: !root.uiBridge.waveformComplete
        && root.uiBridge.waveformCoverageSeconds <= 0

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
        sourcePath: root.visible ? root.uiBridge.currentTrackPath : ""
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
            && root.interactiveOverlaysVisible
        showFpsOverlay: root.uiBridge.showFps
        reservedCorner: root.reservedCorner
        viewMode: root.uiBridge.spectrogramViewMode
        channelCountHint: Math.max(1, root.uiBridge.currentTrackChannels)
        markerSeconds: root.markerSeconds
        // Per channel: a 64-bit mask loses its low bits as a QML number.
        mutedChannels: root.uiBridge.mutedChannels || []
        soloedChannels: root.uiBridge.soloedChannels || []
        // The waveform plot set: on the view colour, in the seek bar's
        // waveform colour.
        backgroundColor: root.plotColors.plotBg
        waveformColor: root.uiPalette.wave
        mutedWaveformColor: ColorUtils.mixColor(root.uiPalette.wave, root.plotColors.plotBg, 0.65)
        centerLineColor: ColorUtils.withAlpha(root.uiPalette.text, 0.08)
        laneSeamColor: root.plotColors.plotLaneSeam
        // Dark: a light core in a black casing; light: a dark core in white.
        playheadColor: root.uiPalette.themeIsDark ? "#e8e8e8" : "#202326"
        playheadCasingColor: root.uiPalette.themeIsDark
            ? Qt.rgba(0, 0, 0, 0.6) : Qt.rgba(1, 1, 1, 0.85)
        onSeekRequested: function(seconds) {
            if (root.seekCommitted) {
                root.seekCommitted(seconds)
            }
        }

        // The only hover handler over the waveform, and on the item itself.
        // The item takes hover for its crosshair, so a handler on a parent
        // never hears of the pointer. A hovered handler on an item above,
        // even one that lets hover through, keeps it from the item instead.
        HoverHandler {
            id: waveformHover
            objectName: "waveformSurfaceHoverHandler"
            cursorShape: root.interactiveOverlaysVisible ? Qt.ArrowCursor : Qt.BlankCursor
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
