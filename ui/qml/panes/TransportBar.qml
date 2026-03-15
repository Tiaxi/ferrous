import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils

ToolBar {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var previousAction
    required property var playAction
    required property var pauseAction
    required property var stopAction
    required property var nextAction
    required property bool themeIsDark
    required property bool volumeMuted
    required property real displayedPositionSeconds
    required property var toggleMutedVolume
    required property var setAppVolume
    required property var normalizedVolumeValue
    required property var seekCommitted

    readonly property alias seekPressed: seekSlider.pressed

    implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
    leftPadding: 8
    rightPadding: 12
    topPadding: 4
    bottomPadding: 4

    contentItem: RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.leftPadding
        anchors.rightMargin: root.rightPadding
        anchors.topMargin: root.topPadding
        anchors.bottomMargin: root.bottomPadding
        spacing: 8

        RowLayout {
            spacing: 2
            ToolButton { action: root.previousAction; display: AbstractButton.IconOnly }
            ToolButton { action: root.playAction; display: AbstractButton.IconOnly }
            ToolButton { action: root.pauseAction; display: AbstractButton.IconOnly }
            ToolButton { action: root.stopAction; display: AbstractButton.IconOnly }
            ToolButton { action: root.nextAction; display: AbstractButton.IconOnly }
        }

        Slider {
            id: seekSlider
            Layout.fillWidth: true
            from: 0
            to: Math.max(root.uiBridge.durationSeconds, 1.0)
            readonly property bool durationKnown: root.uiBridge.durationSeconds > 1.0
            readonly property bool seekAllowed: durationKnown
                && root.uiBridge.playbackState !== "Stopped"
            readonly property real stableVisualPosition: seekAllowed ? visualPosition : 0.0

            enabled: seekAllowed
            stepSize: 0

            onPressedChanged: {
                if (!pressed && seekAllowed) {
                    root.seekCommitted(value)
                }
            }

            background: Item {
                implicitHeight: 24
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    anchors.fill: parent
                    color: "white"
                    border.color: "#a0a9b3"
                    radius: 1
                }

                WaveformItem {
                    id: waveformItem
                    anchors.fill: parent
                    anchors.margins: 1
                    visible: root.uiBridge.playbackState !== "Stopped"
                    peaksData: root.uiBridge.playbackState === "Stopped"
                        ? ""
                        : root.uiBridge.waveformPeaksPacked
                    generatedSeconds: root.uiBridge.waveformCoverageSeconds
                    waveformComplete: root.uiBridge.waveformComplete
                    positionSeconds: root.displayedPositionSeconds
                    durationSeconds: root.uiBridge.durationSeconds
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    visible: seekSlider.seekAllowed
                    width: Math.round(parent.width * seekSlider.stableVisualPosition)
                    color: Qt.rgba(120 / 255, 190 / 255, 1.0, 0.26)
                }

                Rectangle {
                    visible: seekSlider.seekAllowed
                    width: 1
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: Math.round(seekSlider.stableVisualPosition * (parent.width - 1))
                    color: "#2f7cd6"
                }
            }

            handle: Rectangle {
                visible: seekSlider.seekAllowed
                x: seekSlider.leftPadding + seekSlider.stableVisualPosition
                    * (seekSlider.availableWidth - width)
                y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                implicitWidth: 3
                implicitHeight: seekSlider.height - 4
                radius: 1
                color: "#2f7cd6"
                border.color: "#1f5aa7"
            }

            Item {
                id: seekDragOverlay
                visible: seekSlider.pressed && seekSlider.seekAllowed
                z: 20
                property real playheadX: seekSlider.leftPadding
                    + seekSlider.stableVisualPosition * seekSlider.availableWidth
                property real leftCandidateX: playheadX - width - 8
                property real rightCandidateX: playheadX + 8
                width: dragTimeLabel.implicitWidth + 14
                height: Math.max(18, seekSlider.availableHeight - 4)
                y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                x: {
                    const minX = 2
                    const maxX = seekSlider.width - width - 2
                    if (leftCandidateX >= minX) {
                        return Math.min(maxX, leftCandidateX)
                    }
                    return Math.max(minX, Math.min(maxX, rightCandidateX))
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 2
                    color: Qt.rgba(52 / 255, 137 / 255, 235 / 255, 0.76)
                    border.color: Qt.rgba(198 / 255, 229 / 255, 1.0, 0.52)

                    Label {
                        id: dragTimeLabel
                        anchors.centerIn: parent
                        text: FormatUtils.formatSeekTime(seekSlider.value)
                        color: "white"
                    }
                }
            }
        }

        Binding {
            target: seekSlider
            property: "value"
            value: seekSlider.durationKnown ? root.displayedPositionSeconds : 0
            when: !seekSlider.pressed
        }

        Label {
            text: root.uiBridge.positionText + "/" + root.uiBridge.durationText
            horizontalAlignment: Text.AlignHCenter
            Layout.preferredWidth: 96
            Layout.alignment: Qt.AlignVCenter
        }

        ToolButton {
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            Layout.alignment: Qt.AlignVCenter
            display: AbstractButton.IconOnly
            flat: true
            icon.name: (root.volumeMuted || root.normalizedVolumeValue(root.uiBridge.volume) <= 0.0001)
                ? "audio-volume-muted"
                : "audio-volume-high"
            icon.color: ColorUtils.mixColor(
                root.uiPalette.uiTextColor,
                "#ffffff",
                root.themeIsDark ? 0.16 : 0.04)
            onClicked: root.toggleMutedVolume()
        }

        Slider {
            id: volumeSlider
            Layout.preferredWidth: 140
            from: 0
            to: 1
            stepSize: 0
            onMoved: root.setAppVolume(value)
            onPressedChanged: {
                if (!pressed) {
                    root.setAppVolume(value)
                }
            }
        }

        Binding {
            target: volumeSlider
            property: "value"
            value: root.uiBridge.volume
            when: !volumeSlider.pressed
        }
    }
}
