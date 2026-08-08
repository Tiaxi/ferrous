// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0

Item {
    id: root

    required property var uiBridge
    property double positionSeconds: 0
    property var seekCommitted: null
    property bool interactiveOverlaysVisible: true
    property var pointerActivity: null
    readonly property alias waveformItem: waveform

    readonly property var standardChannelLabels: [
        ["M"], ["L", "R"], ["L", "R", "C"], ["L", "R", "Ls", "Rs"],
        ["L", "R", "C", "Ls", "Rs"], ["L", "R", "C", "LFE", "Ls", "Rs"],
        ["L", "R", "C", "LFE", "Ls", "Rs", "Lrs"],
        ["L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs"]
    ]

    function channelLabel(index) {
        const count = waveform.channelCount
        if (count > 0 && count <= standardChannelLabels.length) {
            return standardChannelLabels[count - 1][index] || (index + 1).toString()
        }
        return (index + 1).toString()
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
        positionSeconds: root.positionSeconds
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
                        font.pixelSize: 12
                    }
                }

                Rectangle {
                    property bool active: root.uiBridge.isChannelMuted(index)
                    visible: root.interactiveOverlaysVisible
                        && (root.uiBridge.channelButtonsVisibility === 2
                            || (root.uiBridge.channelButtonsVisibility === 1
                                && channelHover.hovered))
                    width: 24
                    height: 18
                    radius: 3
                    color: active ? "#7d3434" : Qt.rgba(0, 0, 0, 0.5)
                    border.color: Qt.rgba(1, 1, 1, 0.16)
                    Text { anchors.centerIn: parent; text: "M"; color: "#e2e5e3"; font.pixelSize: 10 }
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
                    width: 24
                    height: 18
                    radius: 3
                    color: active ? "#706522" : Qt.rgba(0, 0, 0, 0.5)
                    border.color: Qt.rgba(1, 1, 1, 0.16)
                    Text { anchors.centerIn: parent; text: "S"; color: "#e2e5e3"; font.pixelSize: 10 }
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
            anchors.centerIn: parent
            text: waveform.samplePointsVisible
                ? "Sample view"
                : waveform.zoomLevel.toFixed(waveform.zoomLevel < 10 ? 1 : 0) + "×"
            color: "#c8d0cb"
            font.pixelSize: 10
        }
    }
}
