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
    property var pointerActivity: null
    // What the viewer keeps clear on the right of the mode switch.
    property real trailingInset: 0
    // What the viewer keeps clear along the top edge, under its title strip:
    // the mode switch and the lane heads keep below it.
    property real topInset: 0
    // Whether the plot waits for the track's analysis.
    property bool analysing: false
    // Where the plot draws markerSeconds, NaN while it does not.
    property real markerX: NaN
    // Opens Recent Issues.
    property var openIssues: function() {}

    readonly property var plotColors: root.uiPalette.plotColors(root.visualizationMode)
    readonly property bool meterShown: root.uiBridge.showLevelMeter
    // The meter takes 10 px of the plot's width while it is shown, also
    // when nothing plays: the plot narrows only when the option changes.
    readonly property real meterWidth: root.meterShown ? levelMeter.implicitWidth : 0
    readonly property real plotWidth: Math.max(0, root.width - root.meterWidth)
    readonly property bool trackLoaded: (root.uiBridge.currentTrackPath || "").length > 0
        && (root.uiBridge.playbackState || "Stopped") !== "Stopped"
    // The time where the playing file is cut off, or -1.
    readonly property real cutSeconds: root.uiBridge.waveformEndsEarly
        ? root.uiBridge.waveformCoverageSeconds : -1
    // What the mode switch takes of the top right corner, with the gap
    // beside it, for the plot's own overlays to keep clear of.
    readonly property size reservedCorner: Qt.size(
        modeSwitch.anchors.rightMargin + modeSwitch.width,
        modeSwitch.anchors.topMargin + modeSwitch.height)

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

    // The mode switch comes first in the Tab order, then each lane's Mute
    // and Solo. It shows while the pointer is over the pane or keyboard
    // focus is on it, as Mute and Solo do: in rolling mode, its corner is
    // the newest part of the plot.
    Components.PlotModeSwitch {
        id: modeSwitch
        z: 2
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: root.topInset + 4
        anchors.rightMargin: (root.meterShown ? root.meterWidth + 6 : 4) + root.trailingInset
        visible: root.controlsVisible
        revealed: root.paneHovered || root.hoveredLane >= 0 || modeSwitch.pointerHovered
            || modeSwitch.focusInside
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        selectedMode: root.visualizationMode
        setMode: root.setVisualizationMode
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

    // The lane labels, with Mute and Solo.
    Repeater {
        id: laneHeads
        model: root.laneCount

        delegate: Components.PlotLaneHead {
            required property int index
            objectName: "plotLaneHead"
            x: 5
            y: Math.max(root.laneTop(index), root.topInset) + 5
            visible: label.length > 0
            plotColors: root.plotColors
            uiPalette: root.uiPalette
            label: root.laneLabels[index] || ""
            channelIndex: index
            muted: (root.uiBridge.mutedChannels || [])[index] === true
            soloed: (root.uiBridge.soloedChannels || [])[index] === true
            buttonsVisibility: root.controlsVisible ? root.uiBridge.channelButtonsVisibility : 0
            laneHovered: root.hoveredLane === index
            pointerActivity: root.pointerActivity
            toggleMute: function() { root.uiBridge.toggleChannelMute(index) }
            toggleSolo: function() { root.uiBridge.soloChannel(index) }
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

    Label {
        objectName: "plotAnalysingText"
        anchors.centerIn: parent
        anchors.horizontalCenterOffset: -root.meterWidth / 2
        visible: root._analysingShown
        text: I18n.i18nc("@info:status the visualization waits for its data", "Analysing…")
        font: root.uiPalette.baseFont
        color: root.plotColors.plotTextSecondary
    }

    // A file that ends early: centred in the empty part of the plot after
    // the cut, with the Recent Issues entry it matches.
    Item {
        id: endsEarly
        objectName: "plotEndsEarly"
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

                Label {
                    objectName: "plotEndsEarlyText"
                    text: I18n.i18nc("@info %1 is a time in the track, such as 4:02",
                                     "The file ends at %1.", root.formatCut(root.cutSeconds))
                    font: root.uiPalette.baseFont
                    color: root.plotColors.plotTextSecondary
                }
            }

            Label {
                id: issuesLink
                objectName: "plotEndsEarlyLink"
                anchors.horizontalCenter: parent.horizontalCenter
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
        // Playing: paused or stopped, the column is an empty trough, still
        // reserved.
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
        troughColor: root.plotColors.meterTrough
        fillColor: root.plotColors.meterFill
        peakColor: root.plotColors.meterPeak
        clipOffColor: root.plotColors.meterClipOff
        clipColor: root.uiPalette.negative
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
