// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ChannelLabels.js" as ChannelLabels

Item {
    id: root

    required property var uiBridge
    // Where playback is; see PlaybackClock.
    property var clock: null
    property var seekCommitted: null
    property bool interactiveOverlaysVisible: true
    property var pointerActivity: null
    readonly property alias waveformItem: waveform

    Accessible.role: Accessible.Graphic
    Accessible.name: "Waveform"
    Accessible.description: "Amplitude of the playing track over time"

    function channelLabel(index) {
        return ChannelLabels.label(waveform.channelCount, index,
                                   root.uiBridge.currentTrackChannelLabels || [])
    }

    // Whether a mask of muted channels has `index`. QML numbers hold 64-bit
    // masks only to 53 bits, so this divides instead of shifting 32-bit ints.
    function channelInMask(mask, index) {
        return Math.floor(mask / Math.pow(2, index)) % 2 === 1
    }

    Rectangle {
        anchors.fill: parent
        color: "#050907"
    }

    WaveformEditorItem {
        id: waveform
        objectName: "waveformEditorItem"
        anchors.fill: parent
        sourcePath: root.visible ? root.uiBridge.currentTrackPath : ""
        overviewData: root.uiBridge.waveformPeaksPacked
        overviewCoverageSeconds: root.uiBridge.waveformCoverageSeconds
        overviewComplete: root.uiBridge.waveformComplete
        clock: root.clock
        durationSeconds: root.uiBridge.durationSeconds
        playing: (root.uiBridge.playbackState || "") === "Playing"
        zoomEnabled: root.uiBridge.spectrogramZoomEnabled
        gridEnabled: root.uiBridge.showSpectrogramScale
        crosshairEnabled: root.uiBridge.showSpectrogramCrosshair
            && root.interactiveOverlaysVisible
        showFpsOverlay: root.uiBridge.showFps
        viewMode: root.uiBridge.spectrogramViewMode
        channelCountHint: Math.max(1, root.uiBridge.currentTrackChannels)
        mutedChannelsMask: root.uiBridge.mutedChannelsMask
        soloedChannel: root.uiBridge.soloedChannel
        onSeekRequested: function(seconds) {
            if (root.seekCommitted) {
                root.seekCommitted(seconds)
            }
        }
    }

    HoverHandler {
        id: waveformHover
        objectName: "waveformSurfaceHoverHandler"
        cursorShape: root.interactiveOverlaysVisible ? Qt.ArrowCursor : Qt.BlankCursor
        onPointChanged: {
            waveform.setHoverPosition(point.position.x, point.position.y, hovered)
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
        onHoveredChanged: waveform.setHoverPosition(
            point.position.x, point.position.y, hovered)
    }

    Repeater {
        model: waveform.viewMode === 1 ? waveform.channelCount : 0

        delegate: Item {
            required property int index
            width: root.width
            height: root.height / Math.max(1, waveform.channelCount)
            y: index * height

            HoverHandler {
                id: channelHover
            }

            Row {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 8
                spacing: 4

                Rectangle {
                    width: channelText.implicitWidth + 8
                    height: channelText.implicitHeight + 2
                    radius: 3
                    color: Qt.rgba(0, 0, 0, 0.46)
                    Text {
                        id: channelText
                        anchors.centerIn: parent
                        text: root.channelLabel(index)
                        color: "#d6ded9"
                        font: Kirigami.Theme.smallFont
                    }
                }

                Rectangle {
                    objectName: "waveformChannelMuteButton"
                    readonly property int channelIndex: index
                    // The mask notifies when it changes; isChannelMuted()
                    // does not.
                    property bool active: root.channelInMask(waveform.mutedChannelsMask, index)
                    visible: root.interactiveOverlaysVisible
                        && (root.uiBridge.channelButtonsVisibility === 2
                            || (root.uiBridge.channelButtonsVisibility === 1
                                && channelHover.hovered))
                    width: Math.max(muteText.implicitWidth + 10, height * 4 / 3)
                    height: muteText.implicitHeight + 4
                    radius: 3
                    color: active ? "#7d3434" : Qt.rgba(0, 0, 0, 0.5)
                    border.color: Qt.rgba(1, 1, 1, 0.16)
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: "Mute " + root.channelLabel(index)
                    Accessible.checkable: true
                    Accessible.checked: active
                    Accessible.onPressAction: root.uiBridge.toggleChannelMute(index)
                    Text {
                        id: muteText
                        anchors.centerIn: parent
                        text: "M"
                        color: "#e2e5e3"
                        font: Kirigami.Theme.smallFont
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.uiBridge.toggleChannelMute(index)
                    }
                }

                Rectangle {
                    property bool active: root.uiBridge.soloedChannel === index
                    visible: root.interactiveOverlaysVisible
                        && (root.uiBridge.channelButtonsVisibility === 2
                            || (root.uiBridge.channelButtonsVisibility === 1
                                && channelHover.hovered))
                    width: Math.max(soloText.implicitWidth + 10, height * 4 / 3)
                    height: soloText.implicitHeight + 4
                    radius: 3
                    color: active ? "#706522" : Qt.rgba(0, 0, 0, 0.5)
                    border.color: Qt.rgba(1, 1, 1, 0.16)
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: "Solo " + root.channelLabel(index)
                    Accessible.checkable: true
                    Accessible.checked: active
                    Accessible.onPressAction: root.uiBridge.soloChannel(index)
                    Text {
                        id: soloText
                        anchors.centerIn: parent
                        text: "S"
                        color: "#e2e5e3"
                        font: Kirigami.Theme.smallFont
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.uiBridge.soloChannel(index)
                    }
                }
            }
        }
    }

    Rectangle {
        objectName: "waveformViewportDurationIndicator"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 8
        visible: root.interactiveOverlaysVisible && waveform.zoomLevel > 1.001
        width: zoomText.implicitWidth + 10
        height: zoomText.implicitHeight + 5
        radius: 3
        color: Qt.rgba(0, 0, 0, 0.62)
        border.color: Qt.rgba(1, 1, 1, 0.14)
        Text {
            id: zoomText
            objectName: "waveformViewportDurationText"
            anchors.centerIn: parent
            text: waveform.formatViewportDuration(
                waveform.durationSeconds / Math.max(1, waveform.zoomLevel))
            color: "#c8d0cb"
            font: Kirigami.Theme.smallFont
        }
    }
}
