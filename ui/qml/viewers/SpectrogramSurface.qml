// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Window 2.15
import FerrousUi 1.0
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ChannelLabels.js" as ChannelLabels
import "../logic/PlotLanes.js" as PlotLanes

Item {
    id: root

    required property var uiBridge
    required property var uiPalette
    // Where playback is; see PlaybackClock.
    property var clock: null
    property var seekCommitted: null

    property bool viewerMode: false
    // The plot's width: the level meter's column takes the rest.
    property real plotWidth: root.width
    // A time of the track to follow, such as where its file is cut off, or
    // -1; markerX is where the plot draws it, NaN while it does not.
    property real markerSeconds: -1
    readonly property real markerX: {
        const first = spectrogramRepeater.count > 0 ? spectrogramRepeater.itemAt(0) : null
        return first && first.spectrogramItem ? first.spectrogramItem.markerX : NaN
    }
    // The lanes, each one's label ("" for none) and the one under the pointer.
    readonly property int laneCount: Math.max(1, root.channelDescriptors.length)
    readonly property var laneLabels: {
        const labels = []
        for (let i = 0; i < root.channelDescriptors.length; ++i) {
            labels.push(root.channelDescriptors[i].showLabel
                ? ChannelLabels.label(root.channelDescriptors.length, i,
                                      root.uiBridge.currentTrackChannelLabels || [])
                : "")
        }
        return labels
    }
    property int hoveredLane: -1
    // Whether the plot waits for the track's first columns.
    readonly property bool awaitingData: {
        const first = spectrogramRepeater.count > 0 ? spectrogramRepeater.itemAt(0) : null
        return !(first && first.spectrogramItem && first.spectrogramItem.precomputedReady)
    }
    property bool interactiveOverlaysVisible: true
    // The overlays as they fade with the viewer's controls, from 1 to 0:
    // the crosshair shows until they have faded out. Without a level of
    // the viewer's, they show and hide at once.
    property real interactiveOverlaysLevel: root.interactiveOverlaysVisible ? 1 : 0
    // Whether the lanes draw the playhead; see Main.qml's mode switch.
    property bool playheadVisible: true
    property var pointerActivity: null
    property size reservedCorner: Qt.size(0, 0)
    // What the lowest lane's label and buttons take of it; see PlotLayer.
    property rect laneHeadBounds: Qt.rect(0, 0, 0, 0)
    // The pointer hides once the overlays have faded out, not as they start.
    readonly property int pointerCursorShape: root.interactiveOverlaysVisible || root.interactiveOverlaysLevel > 0
        ? Qt.ArrowCursor
        : Qt.BlankCursor
    property var channelDescriptors: []
    property double _crosshairSharedX: -1.0
    // The zooms the user chose, or 0 for the default zoom, which a view
    // then follows: that of a short track changes with the plot's width and
    // once the track's length is known.
    property double _widgetZoomLevel: 0
    property double _viewerZoomLevel: 0
    // The default zoom: 1, or what stretches a track shorter than the view
    // to the plot's width (Preferences › Appearance).
    readonly property real defaultZoomLevel: {
        const first = spectrogramRepeater.count > 0 ? spectrogramRepeater.itemAt(0) : null
        return first && first.spectrogramItem ? first.spectrogramItem.defaultZoomLevel : 1.0
    }
    // The deepest zoom: 16, or the default where that is deeper. A chosen
    // zoom deeper than it, such as a short track's kept through a rolling
    // gapless transition to a long one, comes back to it.
    readonly property real maximumZoomLevel: {
        const first = spectrogramRepeater.count > 0 ? spectrogramRepeater.itemAt(0) : null
        return first && first.spectrogramItem ? first.spectrogramItem.maximumZoomLevel : 16.0
    }
    readonly property real _chosenZoomLevel: root.viewerMode ? root._viewerZoomLevel : root._widgetZoomLevel
    // The zoom shown: the viewer keeps one of its own.
    readonly property real zoomLevel: root._chosenZoomLevel > 0
        ? Math.min(root._chosenZoomLevel, root.maximumZoomLevel) : root.defaultZoomLevel
    onMaximumZoomLevelChanged: {
        // The zoom shown keeps to the new limit from now on.
        if (root.viewerMode) {
            if (root._viewerZoomLevel > root.maximumZoomLevel) {
                root._viewerZoomLevel = root.maximumZoomLevel
            }
        } else if (root._widgetZoomLevel > root.maximumZoomLevel) {
            root._widgetZoomLevel = root.maximumZoomLevel
        }
    }

    // Both zooms back to the default, as a middle click does.
    function resetZoom() {
        root._widgetZoomLevel = 0
        root._viewerZoomLevel = 0
    }

    // Debounced backend restart after significant width changes
    // (entering/exiting fullscreen).  The timer lets the geometry
    // settle before sending — fullscreen transitions can produce
    // multiple intermediate sizes.
    property int _widthRestartRef: 0
    Timer {
        id: widthSettleTimer
        interval: 100
        repeat: false
        onTriggered: {
            // The restart decodes for the width it is for.
            root.sendBackendWidth()
            root.uiBridge.setSpectrogramZoomLevel(root.zoomLevel)
        }
    }

    // The backend sizes the lookahead of its next session from this width,
    // and a wider one can restart its decoding, so a splitter drag
    // must not send it every pixel. While the width changes, it gets the
    // latest width every 100 ms (enough for a lookahead of seconds), and
    // the final one at most 100 ms after the change stops.
    property int _pendingBackendWidth: 0
    property int _sentBackendWidth: 0
    Timer {
        id: backendWidthTimer
        interval: 100
        repeat: false
        onTriggered: root.sendBackendWidth()
    }

    function reportBackendWidth(width) {
        root._pendingBackendWidth = width
        if (!backendWidthTimer.running) {
            backendWidthTimer.start()
        }
    }

    function sendBackendWidth() {
        backendWidthTimer.stop()
        if (root.uiBridge
                && root._pendingBackendWidth > 0
                && root._pendingBackendWidth !== root._sentBackendWidth) {
            root._sentBackendWidth = root._pendingBackendWidth
            root.uiBridge.setSpectrogramWidgetWidth(root._pendingBackendWidth)
        }
    }


    function descriptorsForChannelCount(count) {
        const isPerChannel = root.uiBridge.spectrogramViewMode === 1
        const showLabels = isPerChannel && count > 0
        let result = []
        for (let i = 0; i < Math.max(count, 1); ++i) {
            const lbl = ChannelLabels.label(count, i)
            const muted = root.uiBridge.isChannelSilent(i)
            result.push({
                label: lbl,
                showLabel: showLabels && lbl.length > 0,
                muted: muted,
                channelIndex: i
            })
        }
        return result
    }

    function placeholderDescriptors() {
        return descriptorsForChannelCount(0)
    }

    function notePointerActivity(x, y) {
        if (root.pointerActivity) {
            root.pointerActivity(x, y)
        }
    }

    function sameDescriptors(next) {
        if (root.channelDescriptors.length !== next.length) {
            return false
        }
        for (let i = 0; i < next.length; ++i) {
            if (root.channelDescriptors[i].label !== next[i].label
                    || root.channelDescriptors[i].showLabel !== next[i].showLabel
                    || root.channelDescriptors[i].muted !== next[i].muted) {
                return false
            }
        }
        return true
    }

    function syncChannelDescriptors(channels) {
        let next = []
        if (channels && channels.length > 0) {
            const showLabels = root.uiBridge.spectrogramViewMode === 1
            for (let i = 0; i < channels.length; ++i) {
                const labelText = (channels[i].label || "").trim()
                const muted = root.uiBridge.isChannelSilent(i)
                next.push({
                    label: labelText,
                    showLabel: showLabels && labelText.length > 0,
                    muted: muted,
                    channelIndex: i
                })
            }
        }
        if (next.length === 0) {
            next = placeholderDescriptors()
        }
        if (sameDescriptors(next)) {
            return
        }
        // When the count is unchanged, only allow model replacement if no
        // pane has precomputed data — replacing the model destroys Repeater
        // delegates and wipes their precomputed atlases.
        if (next.length === root.channelDescriptors.length) {
            let hasPrecomputed = false
            for (let i = 0; i < spectrogramRepeater.count; ++i) {
                const pane = spectrogramRepeater.itemAt(i)
                if (pane && pane.spectrogramItem && pane.spectrogramItem.precomputedReady) {
                    hasPrecomputed = true
                    break
                }
            }
            if (hasPrecomputed) {
                return
            }
        }
        root.channelDescriptors = next
    }

    function resetForCurrentMode(preserveDescriptors) {
        if (!preserveDescriptors) {
            syncChannelDescriptors([])
        }
        for (let i = 0; i < spectrogramRepeater.count; ++i) {
            const pane = spectrogramRepeater.itemAt(i)
            if (pane && pane.spectrogramItem) {
                pane.spectrogramItem.reset()
            }
        }
    }

    function clearPrecomputedForTrackChange() {
        for (let i = 0; i < spectrogramRepeater.count; ++i) {
            const pane = spectrogramRepeater.itemAt(i)
            if (pane && pane.spectrogramItem) {
                pane.spectrogramItem.clearPrecomputed()
            }
        }
    }

    function haltForCurrentMode() {
        for (let i = 0; i < spectrogramRepeater.count; ++i) {
            const pane = spectrogramRepeater.itemAt(i)
            if (pane && pane.spectrogramItem) {
                pane.spectrogramItem.halt()
            }
        }
    }

    Connections {
        target: root.uiBridge
        function onPrecomputedSpectrogramChannelsReady(channelCount, bufferReset) {
            // Sync pane count to match the chunk's channel count.
            // On buffer_reset (track change), allow shrinking; otherwise
            // only grow to avoid destroying precomputed data mid-track.
            if (bufferReset && channelCount !== spectrogramRepeater.count) {
                root.channelDescriptors = descriptorsForChannelCount(channelCount)
            } else if (channelCount > spectrogramRepeater.count) {
                root.channelDescriptors = descriptorsForChannelCount(channelCount)
            }
        }
    }

    Connections {
        target: root.uiBridge
        function onPlaybackChanged() {
            if (spectrogramRepeater.count <= 0) {
                return
            }
            for (let i = 0; i < spectrogramRepeater.count; ++i) {
                const pane = spectrogramRepeater.itemAt(i)
                if (pane && pane.spectrogramItem) {
                    pane.spectrogramItem.channelMuted = root.uiBridge.isChannelSilent(i)
                }
            }
        }
    }

    Component.onCompleted: resetForCurrentMode()
    // A fullscreen viewer closing right after a resize still reports it.
    Component.onDestruction: sendBackendWidth()

    MouseArea {
        objectName: "spectrogramSurfacePointerArea"
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        cursorShape: root.pointerCursorShape
        onPositionChanged: function(mouse) {
            root.notePointerActivity(mouse.x, mouse.y)
        }
    }

    // The seams between the lanes.
    Rectangle {
        objectName: "spectrogramLaneSeams"
        width: root.plotWidth
        height: root.height
        color: root.uiPalette.spectrogramPlot.plotLaneSeam
    }

    Item {
        width: root.plotWidth
        height: root.height

        Repeater {
            id: spectrogramRepeater
            model: root.channelDescriptors

            delegate: Item {
                property alias spectrogramItem: spectrogramPaneItem

                width: root.plotWidth
                y: PlotLanes.top(index, spectrogramRepeater.count, root.height)
                height: PlotLanes.laneHeight(index, spectrogramRepeater.count, root.height)

                Rectangle {
                    objectName: "spectrogramLaneFloor"
                    anchors.fill: parent
                    color: root.uiPalette.spectrogramPlot.plotBg
                }

                SpectrogramItem {
                    id: spectrogramPaneItem
                    objectName: "spectrogramPaneItem"
                    anchors.fill: parent
                    channelMuted: modelData.muted || false
                    playheadVisible: root.playheadVisible
                    maxColumns: Math.max(Math.floor(width), Screen.desktopAvailableWidth)
                    dbRange: root.uiBridge.dbRange
                    logScale: root.uiBridge.logScale
                    showFpsOverlay: index === 0 ? (root.uiBridge.showFps || spectrogramPaneItem.forceFpsOverlay) : false
                    // A short pane can have the corner reach into the next.
                    reservedCorner: Qt.size(root.reservedCorner.width,
                                            Math.max(0, root.reservedCorner.height - parent.y))
                    sampleRateHz: root.uiBridge.sampleRateHz
                    clock: root.clock
                    playing: (root.uiBridge.playbackState || "") === "Playing"
                    displayMode: root.uiBridge.spectrogramDisplayMode
                    crosshairEnabled: root.uiBridge.showSpectrogramCrosshair
                        && (root.interactiveOverlaysVisible || root.interactiveOverlaysLevel > 0)
                    crosshairOpacity: root.interactiveOverlaysLevel
                    // The crosshair's readout is drawn on the plot: the time
                    // on the bottom edge of the lowest lane.
                    showCrosshairTime: index === spectrogramRepeater.count - 1
                    laneHeadBounds: index === spectrogramRepeater.count - 1
                        ? root.laneHeadBounds : Qt.rect(0, 0, 0, 0)
                    readoutPlateColor: root.uiPalette.spectrogramPlot.plotPlate
                    readoutPlateEdgeColor: root.uiPalette.spectrogramPlot.plotPlateEdge
                    readoutTextColor: root.uiPalette.spectrogramPlot.plotText
                    gridEnabled: root.uiBridge.showSpectrogramScale
                    // At rest, the scale's lines where a session will put
                    // its ticks.
                    analysisFftSize: root.uiBridge.fftSize || 0
                    frequencyCap: root.uiBridge.spectrogramFrequencyCap === true
                    // The time scale runs along the top of the first lane.
                    showTimeLabels: index === 0
                    markerSeconds: index === 0 ? root.markerSeconds : -1
                    crosshairSharedX: root._crosshairSharedX
                    onCrosshairHoverChanged: (x) => { root._crosshairSharedX = x }
                    zoomEnabled: root.uiBridge.spectrogramZoomEnabled
                    stretchShortTracks: root.uiBridge.stretchShortTracks
                    // Each lane at its own default, which its data tells.
                    zoomLevel: root._chosenZoomLevel > 0
                        ? Math.min(root._chosenZoomLevel, spectrogramPaneItem.maximumZoomLevel)
                        : spectrogramPaneItem.defaultZoomLevel
                    onZoomRequested: (newZoomLevel) => {
                        const minZoom = spectrogramPaneItem.minimumZoomLevel()
                        const clamped = Math.max(minZoom,
                            Math.min(spectrogramPaneItem.maximumZoomLevel, newZoomLevel))
                        if (root.viewerMode) {
                            root._viewerZoomLevel = clamped
                        } else {
                            root._widgetZoomLevel = clamped
                        }
                    }
                    onZoomResetRequested: root.resetZoom()
                    onBackendZoomRequested: (level) => {
                        if (index === 0) {
                            // The restart decodes for the width it is for.
                            root.sendBackendWidth()
                            root.uiBridge.setSpectrogramZoomLevel(level)
                        }
                    }
                    Component.onCompleted: {
                        root.uiBridge.registerSpectrogramItem(spectrogramPaneItem, index)
                    }
                    Component.onDestruction: {
                        root.uiBridge.unregisterSpectrogramItem(spectrogramPaneItem)
                    }
                    // Only report width from the first pane (all share the same width).
                    onSpectrogramWidthChanged: (w) => {
                        if (index === 0) {
                            root.reportBackendWidth(w)
                            // On significant width changes (>20%), debounce
                            // a backend restart so the decode window matches.
                            const ref = root._widthRestartRef
                            if (ref > 0) {
                                const ratio = w / ref
                                if (ratio > 1.2 || ratio < 0.8) {
                                    widthSettleTimer.restart()
                                }
                            }
                            root._widthRestartRef = w
                        }
                    }
                    onSeekRequested: (seconds) => {
                        if (root.seekCommitted) {
                            root.seekCommitted(seconds)
                        }
                    }
                    // Pane-level hover detection for M/S button visibility and
                    // viewer pointer-idle tracking.
                    // Attached to SpectrogramItem (not the parent Item) because
                    // SpectrogramItem accepts hover events for crosshair overlay,
                    // so a HoverHandler on the parent never sees hover.
                    // HoverHandler is passive and doesn't interfere with the C++
                    // hoverMoveEvent.
                    HoverHandler {
                        id: paneHover
                        cursorShape: root.pointerCursorShape
                        onHoveredChanged: {
                            if (hovered) {
                                root.hoveredLane = index
                            } else if (root.hoveredLane === index) {
                                root.hoveredLane = -1
                            }
                        }
                        onPointChanged: root.notePointerActivity(
                            point.scenePosition.x, point.scenePosition.y)
                    }
                }
            }
        }
    }
}
