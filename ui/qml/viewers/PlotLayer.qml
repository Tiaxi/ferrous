// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import FerrousUi 1.0
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components
import "../logic/PlotLanes.js" as PlotLanes

// What is drawn on the visualization, in the plot's own colours: the lane
// labels with their Mute and Solo buttons, the mode switch, the level
// meter's column at the right end of each lane, and the pane's states
// (Analysing…, a file that ends early). The plot under it is
// `plotWidth` wide; the meter takes the rest while it is shown. The main
// pane and both viewers show the same layer.
Item {
    id: root
    objectName: "plotLayer"

    required property var uiBridge
    required property var uiPalette
    property var clock: null
    property int visualizationMode: 0
    property var setVisualizationMode: function(mode) {}
    // The lanes of the plot, and each one's label ("" for none).
    property int laneCount: 1
    property var laneLabels: []
    // The lane under the pointer, from the plot, which takes hover; or -1.
    property int hoveredLane: -1
    // Whether the pointer is over the pane, from the item that holds the
    // plot and this layer: a handler here would take hover from the plot.
    property bool paneHovered: false
    // Whether the viewer's controls show; the main pane's always do.
    property bool controlsVisible: true
    // Those controls as they fade (FullscreenControlsAutoHide's level),
    // which the mode switch and the lanes' buttons take as their opacity.
    property real controlsLevel: root.controlsVisible ? 1 : 0
    // In a viewer, whose controls show on the pointer's activity and hide
    // after it as one layer: the mode switch and the lanes' Mute and Solo
    // show whenever they do, on their level alone, rather than each on the
    // pointer over the pane or its lane, a moment apart.
    property bool controlsAutoHide: false
    property var pointerActivity: null
    // What the viewer keeps clear along the top edge, under its title strip:
    // the lane heads keep below it. Their labels do only while the controls
    // show; hidden, each is where its lane alone puts it.
    property real topInset: 0
    // Whether the plot waits for the track's analysis.
    property bool analysing: false
    // Where the plot draws markerSeconds, NaN while it does not.
    property real markerX: NaN
    // Opens Recent Issues.
    property var openIssues: function() {}
    // The shown plot's zoom, 1 at the default, and what resets it. While it
    // is zoomed, a readout and Reset are left of the mode switch in the
    // main pane, and show with it; a viewer has them in its own controls,
    // in the same order.
    property real zoomLevel: 1.0
    property var resetZoom: null
    // Whether a viewer shows the mode switch and the zoom in its own
    // controls, on its corner plate or in its title strip: then they are
    // not on the plot, whose switch and zoom are the main pane's alone.
    property bool controlsInViewer: false
    // Whether the pointer is on the plate of those controls over the plot.
    property bool viewerPlateHovered: false

    // The mode's plot colours, also while the plot is bare, before any
    // audio and while it waits for its data: the floor is the mode's own
    // then too, so the first data never changes it.
    readonly property var plotColors: root.uiPalette.plotColors(root.visualizationMode)
    readonly property bool meterShown: root.uiBridge.showLevelMeter
    // The meter takes its column of the plot's width while it is shown,
    // also when nothing plays: the plot narrows only when the option
    // changes.
    readonly property real meterWidth: root.meterShown ? levelMeter.implicitWidth : 0
    readonly property real plotWidth: Math.max(0, root.width - root.meterWidth)
    readonly property bool trackLoaded: (root.uiBridge.currentTrackPath || "").length > 0
        && (root.uiBridge.playbackState || "Stopped") !== "Stopped"
    // Before any audio: no track, and the plot is its bare floor.
    readonly property bool beforeAudio: (root.uiBridge.currentTrackPath || "").length === 0
    // The time where the playing file is cut off, or -1.
    readonly property real cutSeconds: root.uiBridge.waveformEndsEarly
        ? root.uiBridge.waveformCoverageSeconds : -1
    // What the mode switch takes of the top right corner, with the gap
    // beside it and the zoom readout left of it while that shows, and the
    // plates' 1 px edges outside them, for the plot's own overlays to keep
    // clear of; with them in the viewer's strip, only the meter's column.
    readonly property size reservedCorner: root.controlsInViewer
        ? Qt.size(root.meterWidth, 0)
        : Qt.size(
            modeSwitch.anchors.rightMargin + modeSwitch.width
                + (zoomPlate.visible ? zoomPlate.anchors.rightMargin + zoomPlate.width : 0) + 1,
            modeSwitch.anchors.topMargin + Math.max(modeSwitch.height,
                                                    zoomPlate.visible ? zoomPlate.height : 0) + 1)

    // Lanes shorter than this, such as each of 5.1's in the main window,
    // have compact heads: 72 px with the default font, where a
    // 20 px label and its 28 px buttons would take most of the lane.
    readonly property real compactLaneHeight: 4 * Kirigami.Units.gridUnit
    // The plates' inset from the plot's corners: the lane heads' and the
    // mode switch's.
    readonly property int plateInset: 5
    // Where the pointer is on the plot, in this layer's coordinates, while
    // `pointerKnown`: for the lane labels, which take no hover.
    property point pointerPosition: Qt.point(0, 0)
    property bool pointerKnown: false
    // Whether the pointer is on one of the plot's plates: a lane's label or
    // its buttons, the mode switch or the zoom, or the viewer's own plate.
    readonly property bool plateHovered: {
        if (root.viewerPlateHovered || modeSwitch.pointerHovered
                || (zoomPlate.visible && zoomPlate.pointerHovered))
            return true
        for (let i = 0; i < laneHeads.count; ++i) {
            const head = laneHeads.itemAt(i)
            if (head && head.plateHovered)
                return true
        }
        return false
    }
    // How much of the plot's crosshair and its readout show: none while the
    // pointer is on a plate, where they ran under it and through it,
    // fading over shortDuration, OutCubic; at once with animations off.
    property real crosshairLevel: root.plateHovered ? 0 : 1
    Behavior on crosshairLevel {
        enabled: Kirigami.Units.shortDuration > 0
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
        }
    }

    function laneTop(index) { return PlotLanes.top(index, root.laneCount, root.height) }
    function laneHeight(index) { return PlotLanes.laneHeight(index, root.laneCount, root.height) }

    // "4:02", as the Recent Issues entry says it.
    function formatCut(seconds) {
        const whole = Math.max(0, Math.floor(seconds))
        const hours = Math.floor(whole / 3600)
        const minutes = Math.floor(whole / 60) % 60
        const secs = String(whole % 60).padStart(2, "0")
        return hours > 0
            ? hours + ":" + String(minutes).padStart(2, "0") + ":" + secs
            : minutes + ":" + secs
    }

    // The corner is one row, which shows and hides together: the zoom's
    // plate and the mode switch, while the pointer is on the pane or
    // keyboard focus is on either.
    readonly property bool cornerWanted: root.controlsAutoHide || root.beforeAudio || root.paneHovered
        || root.hoveredLane >= 0 || modeSwitch.pointerHovered || modeSwitch.focusInside
        || zoomPlate.focusInside

    // The mode switch comes first in the Tab order, then each lane's Mute
    // and Solo. It shows while the pointer is over the pane or keyboard
    // focus is on it, as Mute and Solo do: in rolling mode, its corner is
    // the newest part of the plot. Before any audio the plot has nothing
    // to cover, and the switch shows, so a mode can be picked first. A
    // viewer has a switch of its own.
    Components.PlotModeSwitch {
        id: modeSwitch
        z: 2
        anchors.top: parent.top
        anchors.right: parent.right
        // 5 px in, as the lane heads are, their plates' tops on one line
        // with its own.
        anchors.topMargin: root.topInset + root.plateInset
        anchors.rightMargin: (root.meterShown ? root.meterWidth : 0) + root.plateInset
        // Not in a viewer, which has it in its own controls. It comes back
        // to the pane at the level of those; hidden, it takes no pointer.
        visible: !root.controlsInViewer && (root.controlsVisible || root.controlsLevel > 0)
        revealed: root.cornerWanted
        fadeLevel: root.controlsLevel
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        selectedMode: root.visualizationMode
        setMode: root.setVisualizationMode
        pointerActivity: root.pointerActivity
    }

    // The zoom, 4 px left of the mode switch, so that the corner's plates
    // read as one row: while the plot is zoomed, it shows and hides with
    // the switch (shown without it, it floated 68 px from the corner). At
    // rest the seek bar's band is the zoom's cue, and the wheel zooms with
    // the pointer on the pane, where the readout shows as it changes. It
    // keeps its place and its Tab stop while it is hidden.
    Components.PlotZoomPlate {
        id: zoomPlate
        z: 2
        anchors.top: modeSwitch.top
        anchors.right: modeSwitch.left
        anchors.rightMargin: 4
        available: !root.controlsInViewer && (root.controlsVisible || root.controlsLevel > 0)
        revealed: modeSwitch.revealed
        fadeLevel: root.controlsLevel
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        zoomLevel: root.zoomLevel
        resetView: root.resetZoom
        pointerActivity: root.pointerActivity
    }

    // What the lowest lane's head takes of that lane, from its top left,
    // for the plot's crosshair time to keep clear of; empty while it shows
    // none.
    readonly property rect lowestLaneHeadBounds: {
        const index = laneHeads.count - 1
        const head = index >= 0 ? laneHeads.itemAt(index) : null
        if (!head || !head.visible) {
            return Qt.rect(0, 0, 0, 0)
        }
        const bounds = head.occupiedBounds
        return Qt.rect(head.x + bounds.x, head.y - root.laneTop(index) + bounds.y,
                       bounds.width, bounds.height)
    }

    // What each lane's head takes of its lane, from the lane's top left,
    // by the lane's index; empty for a lane that shows none. The lines of
    // a lane at rest keep clear of it, where its label rests with a
    // viewer's controls hidden too, so that they never move with those.
    readonly property var laneHeadBoundsByLane: {
        const bounds = []
        for (let index = 0; index < laneHeads.count; ++index) {
            const head = laneHeads.itemAt(index)
            if (!head || !head.visible) {
                bounds.push(Qt.rect(0, 0, 0, 0))
                continue
            }
            const occupied = head.occupiedBounds
            bounds.push(Qt.rect(head.x + occupied.x,
                                head.y - head.stripShift - root.laneTop(index) + occupied.y,
                                occupied.width, occupied.height + head.stripShift))
        }
        return bounds
    }

    // The lane labels, with Mute and Solo.
    Repeater {
        id: laneHeads
        model: root.laneCount

        delegate: Components.PlotLaneHead {
            id: laneHead
            required property int index
            // How much lower than in the other lanes the head is in this
            // one, as a viewer's title strip reaches into it.
            readonly property real stripShift: Math.max(0, root.topInset - root.laneTop(index))
            objectName: "plotLaneHead"
            z: 1
            x: root.plateInset
            // The buttons' plate 5 px in, as the mode switch, and the label
            // centred on it: the head's own top is the label's.
            y: Math.max(root.laneTop(index), root.topInset) + root.plateInset + plateReach
            visible: label.length > 0
            plotColors: root.plotColors
            uiPalette: root.uiPalette
            label: root.laneLabels[index] || ""
            channelIndex: index
            compact: root.laneHeight(index) < root.compactLaneHeight
            muted: (root.uiBridge.mutedChannels || [])[index] === true
            soloed: (root.uiBridge.soloedChannels || [])[index] === true
            // Before any audio the lanes at rest are named, with nothing
            // to mute or solo yet.
            buttonsVisibility: root.beforeAudio ? 0 : root.uiBridge.channelButtonsVisibility
            controlsShown: root.controlsVisible
            controlsLevel: root.controlsLevel
            revealedWithControls: root.controlsAutoHide
            laneHovered: root.hoveredLane === index
            pointerPosition: root.pointerPosition
            pointerKnown: root.pointerKnown
            pointerActivity: root.pointerActivity
            toggleMute: function() { root.uiBridge.toggleChannelMute(index) }
            toggleSolo: function() { root.uiBridge.soloChannel(index) }
            // The label has two places where the strip reaches into the
            // lane, under the strip and, with the controls hidden, as far
            // into the lane as the others' (left under a strip that was
            // gone, it was stranded 38 px down, in the state a listener sees
            // most). It cross-fades between them on the controls' level, so
            // nothing travels or jumps as the strip shows and hides.
            labelLevel: laneHead.stripShift > 0 ? root.controlsLevel : 1

            Components.PlotPlate {
                objectName: "plotLaneRestLabel"
                y: -laneHead.stripShift
                visible: laneHead.stripShift > 0 && root.controlsLevel < 1
                fadeLevel: 1 - root.controlsLevel
                plotColors: root.plotColors
                uiPalette: root.uiPalette
                text: laneHead.label
                compact: laneHead.compact
                // The head's label is the one that is read.
                Accessible.ignored: true
            }
        }
    }

    // Analysing…: before the plot has the track's first data. Data that
    // comes within a moment, as after a track change or a seek, shows no
    // message, like any busy indicator.
    readonly property bool _awaiting: root.analysing && root.trackLoaded && root.cutSeconds < 0
    property bool _analysingShown: false
    on_AwaitingChanged: {
        if (!root._awaiting) {
            analysingDelay.stop()
            root._analysingShown = false
        } else {
            analysingDelay.restart()
        }
    }
    Timer {
        id: analysingDelay
        interval: Kirigami.Units.humanMoment / 5
        onTriggered: root._analysingShown = root._awaiting
    }
    Component.onCompleted: {
        if (root._awaiting) {
            analysingDelay.restart()
        }
    }

    // Over the plot, a Text rather than a Label: the desktop style's Label
    // has a hover handler, which would take the hover from the view. Unlike
    // a Label, a Text is not read by assistive technology unless it says so.
    Text {
        objectName: "plotAnalysingText"
        z: 1
        anchors.centerIn: parent
        anchors.horizontalCenterOffset: -root.meterWidth / 2
        visible: root._analysingShown
        textFormat: Text.PlainText
        text: I18n.i18nc("@info:status the visualization waits for its data", "Analysing…")
        font: root.uiPalette.baseFont
        color: root.plotColors.plotTextSecondary
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // A file that ends early: centred in the empty part of the plot after
    // the cut, with the Recent Issues entry it matches.
    Item {
        id: endsEarly
        objectName: "plotEndsEarly"
        z: 1
        readonly property real cutEdge: Math.max(0, Math.ceil(root.markerX) + 1)
        x: cutEdge
        width: Math.max(0, root.plotWidth - cutEdge)
        height: root.height
        visible: root.cutSeconds >= 0 && !isNaN(root.markerX)
            && width >= endsEarlyColumn.implicitWidth + 2 * Kirigami.Units.largeSpacing

        Column {
            id: endsEarlyColumn
            anchors.centerIn: parent
            spacing: Kirigami.Units.smallSpacing

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 6

                Kirigami.Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    source: "dialog-warning-symbolic"
                    isMask: true
                    color: root.plotColors.plotTextSecondary
                }

                Text {
                    objectName: "plotEndsEarlyText"
                    textFormat: Text.PlainText
                    text: I18n.i18nc("@info %1 is a time in the track, such as 4:02",
                                     "The file ends at %1.", root.formatCut(root.cutSeconds))
                    font: root.uiPalette.baseFont
                    color: root.plotColors.plotTextSecondary
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            Label {
                id: issuesLink
                objectName: "plotEndsEarlyLink"
                anchors.horizontalCenter: parent.horizontalCenter
                textFormat: Text.PlainText
                text: I18n.i18nc("@action:button", "Show in Recent Issues")
                font.family: root.uiPalette.baseFont.family
                font.pointSize: root.uiPalette.baseFont.pointSize
                font.underline: activeFocus || linkArea.containsMouse
                color: root.plotColors.plotLink
                activeFocusOnTab: true
                Accessible.role: Accessible.Link
                Accessible.name: text
                Accessible.focusable: true
                Accessible.onPressAction: root.openIssues()
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                            || event.key === Qt.Key_Space) {
                        root.openIssues()
                        event.accepted = true
                    }
                }

                MouseArea {
                    id: linkArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openIssues()
                }
            }
        }
    }

    LevelMeterItem {
        id: levelMeter
        objectName: "levelMeter"
        Accessible.role: Accessible.Graphic
        Accessible.name: I18n.i18nc("@label", "Level meter")
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: implicitWidth
        visible: root.meterShown
        laneCount: root.laneCount
        // Playing: paused, stopped or with no track, the column is a trough
        // with the marks' ticks alone, still reserved.
        active: root.trackLoaded && root.uiBridge.connected
            && root.uiBridge.playbackState === "Playing"
        sourcePath: visible && root.trackLoaded ? root.uiBridge.currentTrackPath : ""
        clock: root.clock
        durationSeconds: root.uiBridge.durationSeconds
        playing: root.uiBridge.connected && root.uiBridge.playbackState === "Playing"
        channelCountHint: root.uiBridge.currentTrackChannels > 0 ? root.uiBridge.currentTrackChannels : 2
        readoutWanted: meterLanes.hoveredLane >= 0
        backgroundColor: root.plotColors.plotBg
        seamColor: root.plotColors.plotLaneSeam
        gapColor: root.plotColors.meterSeam
        // Before any audio, since launch or since the queue was cleared
        // (beforeAudio: no track path, the pane at rest with its rest
        // lines), the track is the plot's floor and its marks the rest
        // lines' colour; from the first track on, the lit trough and its
        // ticks, paused and stopped with a track too.
        troughColor: root.beforeAudio ? root.plotColors.meterRestTrough : root.plotColors.meterTrough
        fillColor: root.plotColors.meterFill
        peakColor: root.plotColors.meterPeak
        clipOffColor: root.plotColors.meterClipOff
        clipColor: root.uiPalette.negative
        tickColor: root.beforeAudio ? root.plotColors.meterRestTick : root.plotColors.meterTick
        topSeamColor: root.plotColors.meterTopSeam
        // On the waveform, whose colour the bar has, the bar is framed by
        // its trough, so it reads as an instrument rather than as the
        // waveform's tail. Its marks and scale are the same in both modes.
        framed: root.visualizationMode === 1
        // Out to the pane's edge, in the main window and in the viewers
        // alike: its bar is far wider than the queue's scrollbar above it.
    }

    // Each lane's column, for its tooltip and for assistive technology:
    // the channel and its levels in dB.
    Repeater {
        id: meterLanes
        property int hoveredLane: -1
        model: root.meterShown ? root.laneCount : 0

        delegate: Item {
            id: meterLane
            required property int index
            readonly property var readout: {
                levelMeter.readoutRevision
                return levelMeter.laneReadout(index)
            }
            readonly property string channel: root.laneLabels[index] || ""
            readonly property string levelsText: {
                const level = meterLane.readout.level
                const peak = meterLane.readout.peak
                const parts = [I18n.i18nc("@info:tooltip level meter; %1 is a level", "Level %1",
                                          root.formatDecibels(level)),
                               I18n.i18nc("@info:tooltip level meter; %1 is a level", "peak %1",
                                          root.formatDecibels(peak))]
                if (meterLane.readout.clipping) {
                    parts.push(I18n.i18nc("@info:tooltip level meter", "clipping"))
                }
                return parts.join(", ")
            }
            // The value interface of assistive technology reads these.
            readonly property real value: isFinite(meterLane.readout.level)
                ? Math.round(meterLane.readout.level * 10) / 10 : -60
            readonly property real from: -60
            readonly property real to: 0

            objectName: "levelMeterLane"
            x: root.width - levelMeter.width
            y: root.laneTop(index)
            width: levelMeter.width
            height: root.laneHeight(index)

            Accessible.role: Accessible.ProgressBar
            Accessible.name: meterLane.channel.length > 0
                ? I18n.i18nc("@label %1 is a channel", "Level of %1", meterLane.channel)
                : I18n.i18nc("@label", "Level")
            Accessible.description: meterLane.levelsText

            HoverHandler {
                id: meterHover
                onHoveredChanged: {
                    if (hovered) {
                        meterLanes.hoveredLane = meterLane.index
                    } else if (meterLanes.hoveredLane === meterLane.index) {
                        meterLanes.hoveredLane = -1
                    }
                }
            }

            ToolTip.visible: meterHover.hovered
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: meterLane.channel.length > 0
                ? I18n.i18nc("@info:tooltip %1 is a channel, %2 its levels", "%1: %2",
                             meterLane.channel, meterLane.levelsText)
                : meterLane.levelsText
        }
    }

    // "−12.3 dB", or "−∞ dB" below the meter's floor.
    function formatDecibels(db) {
        if (!isFinite(db)) {
            return I18n.i18nc("@info:tooltip level in decibels; %1 is a number", "%1 dB",
                              "−∞")
        }
        const rounded = Math.round(db * 10) / 10
        const text = Math.abs(rounded).toFixed(1)
        return I18n.i18nc("@info:tooltip level in decibels; %1 is a number", "%1 dB",
                          rounded < 0 ? "−" + text : text)
    }
}
