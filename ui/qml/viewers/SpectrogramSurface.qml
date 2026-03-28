// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import FerrousUi 1.0

Item {
    id: root

    required property var uiBridge
    property double positionSeconds: 0

    property var channelDescriptors: []

    // Standard channel labels for common layouts.
    readonly property var standardChannelLabels: [
        ["M"],
        ["L", "R"],
        ["L", "R", "C"],
        ["L", "R", "Ls", "Rs"],
        ["L", "R", "C", "Ls", "Rs"],
        ["L", "R", "C", "LFE", "Ls", "Rs"],
        ["L", "R", "C", "LFE", "Ls", "Rs", "Lrs"],
        ["L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs"]
    ]

    function descriptorsForChannelCount(count) {
        const isPerChannel = root.uiBridge.spectrogramViewMode === 1
        const showLabels = isPerChannel && count > 0
        const labels = count > 0 && count <= standardChannelLabels.length
            ? standardChannelLabels[count - 1]
            : null
        let result = []
        for (let i = 0; i < Math.max(count, 1); ++i) {
            const lbl = labels ? labels[i] || "" : (count === 0 ? "M" : "")
            result.push({ label: lbl, showLabel: showLabels && lbl.length > 0 })
        }
        return result
    }

    function placeholderDescriptors() {
        return descriptorsForChannelCount(0)
    }

    function sameDescriptors(next) {
        if (root.channelDescriptors.length !== next.length) {
            return false
        }
        for (let i = 0; i < next.length; ++i) {
            if (root.channelDescriptors[i].label !== next[i].label
                    || root.channelDescriptors[i].showLabel !== next[i].showLabel) {
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
                next.push({
                    label: labelText,
                    showLabel: showLabels && labelText.length > 0
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
        function onPrecomputedSpectrogramChunkReady(data, bins, channelCount, columns,
                                                     startIndex, totalEstimate, sampleRate,
                                                     hopSize, coverage, complete, bufferReset,
                                                     clearHistory, trackToken) {
            // Sync pane count to match the chunk's channel count.
            // On buffer_reset (track change), allow shrinking; otherwise
            // only grow to avoid destroying precomputed data mid-track.
            if (bufferReset && channelCount !== spectrogramRepeater.count) {
                root.channelDescriptors = descriptorsForChannelCount(channelCount)
            } else if (channelCount > spectrogramRepeater.count) {
                root.channelDescriptors = descriptorsForChannelCount(channelCount)
            }

            const paneCount = spectrogramRepeater.count
            for (let ch = 0; ch < channelCount; ++ch) {
                if (ch >= paneCount) {
                    break
                }
                const pane = spectrogramRepeater.itemAt(ch)
                if (pane && pane.spectrogramItem) {
                    pane.spectrogramItem.feedPrecomputedChunk(
                        data, bins, ch, columns, startIndex,
                        totalEstimate, sampleRate, hopSize, complete,
                        bufferReset, trackToken, clearHistory)
                }
            }
        }
    }

    Component.onCompleted: resetForCurrentMode()

    ColumnLayout {
        anchors.fill: parent
        spacing: root.channelDescriptors.length > 1 ? 2 : 0

        Repeater {
            id: spectrogramRepeater
            model: root.channelDescriptors

            delegate: Item {
                property alias spectrogramItem: spectrogramPaneItem

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: 1
                Layout.minimumHeight: 0

                Rectangle {
                    anchors.fill: parent
                    color: "#0b0b0f"
                }

                SpectrogramItem {
                    id: spectrogramPaneItem
                    anchors.fill: parent
                    maxColumns: Math.max(Math.floor(width), Screen.desktopAvailableWidth)
                    dbRange: root.uiBridge.dbRange
                    logScale: root.uiBridge.logScale
                    showFpsOverlay: index === 0 ? (root.uiBridge.showFps || spectrogramPaneItem.forceFpsOverlay) : false
                    sampleRateHz: root.uiBridge.sampleRateHz
                    positionSeconds: root.positionSeconds
                    playing: (root.uiBridge.playbackState || "") === "Playing"
                    displayMode: root.uiBridge.spectrogramDisplayMode
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 8
                    width: labelText.implicitWidth + 8
                    height: labelText.implicitHeight + 2
                    radius: 4
                    color: Qt.rgba(0.0, 0.0, 0.0, 0.18)
                    visible: modelData.showLabel

                    Text {
                        id: labelText
                        anchors.centerIn: parent
                        text: modelData.label
                        color: Qt.rgba(0.90, 0.93, 0.98, 0.74)
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                }
            }
        }
    }
}
