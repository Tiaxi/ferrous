import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import FerrousUi 1.0

Item {
    id: root

    required property var uiBridge

    property var channelDescriptors: []
    property var pendingPackedBatches: []
    property bool pendingPackedFlushScheduled: false

    function placeholderDescriptors() {
        return root.uiBridge.spectrogramViewMode === 1
            ? [{ label: "M", showLabel: true }]
            : [{ label: "", showLabel: false }]
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
        if (!sameDescriptors(next)) {
            root.channelDescriptors = next
        }
    }

    function schedulePendingPackedFlush() {
        if (root.pendingPackedFlushScheduled) {
            return
        }
        root.pendingPackedFlushScheduled = true
        Qt.callLater(function() {
            root.pendingPackedFlushScheduled = false
            root.flushPendingPackedDeltas()
        })
    }

    function flushPendingPackedDeltas() {
        if (!root.pendingPackedBatches || root.pendingPackedBatches.length === 0) {
            return
        }

        const batch = root.pendingPackedBatches[0]
        const channels = batch ? batch.channels : null
        const seedHistory = batch ? batch.seedHistory === true : false
        if (!channels || channels.length === 0) {
            root.pendingPackedBatches.shift()
            if (root.pendingPackedBatches.length > 0) {
                schedulePendingPackedFlush()
            }
            return
        }

        syncChannelDescriptors(channels)
        if (spectrogramRepeater.count < channels.length) {
            schedulePendingPackedFlush()
            return
        }

        for (let i = 0; i < channels.length; ++i) {
            const pane = spectrogramRepeater.itemAt(i)
            if (!pane || !pane.spectrogramItem) {
                schedulePendingPackedFlush()
                return
            }
        }

        root.pendingPackedBatches.shift()
        for (let i = 0; i < channels.length; ++i) {
            const pane = spectrogramRepeater.itemAt(i)
            const channel = channels[i]
            if (!pane || !pane.spectrogramItem || !channel) {
                continue
            }
            if ((channel.rows || 0) > 0 && (channel.bins || 0) > 0) {
                pane.spectrogramItem.appendPackedRows(
                            channel.data,
                            channel.rows,
                            channel.bins,
                            seedHistory)
            }
        }

        if (root.pendingPackedBatches.length > 0) {
            schedulePendingPackedFlush()
        }
    }

    function resetForCurrentMode(preserveDescriptors) {
        root.pendingPackedBatches = []
        root.pendingPackedFlushScheduled = false
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

    function haltForCurrentMode() {
        root.pendingPackedBatches = []
        root.pendingPackedFlushScheduled = false
        for (let i = 0; i < spectrogramRepeater.count; ++i) {
            const pane = spectrogramRepeater.itemAt(i)
            if (pane && pane.spectrogramItem) {
                pane.spectrogramItem.halt()
            }
        }
    }

    function appendPackedDelta(channels, seedHistory) {
        if (!channels || channels.length === 0) {
            return
        }
        root.pendingPackedBatches.push({
            channels: channels,
            seedHistory: seedHistory === true
        })
        schedulePendingPackedFlush()
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
                    showFpsOverlay: index === 0 ? root.uiBridge.showFps : false
                    sampleRateHz: root.uiBridge.sampleRateHz
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
