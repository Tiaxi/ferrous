// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils

ToolBar {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var previousAction
    required property var playAction
    required property var pauseAction
    required property var stopAction
    required property var nextAction
    required property bool volumeMuted
    // How far the arrow keys move the seek bar.
    required property real keyboardSeekStepSeconds
    required property real displayedPositionSeconds
    property string positionText: "00:00"
    required property var toggleMutedVolume
    required property var setAppVolume
    required property var normalizedVolumeValue
    required property var seekCommitted

    readonly property alias seekPressed: seekSlider.pressed
    readonly property bool volumeSilent: root.volumeMuted
        || root.normalizedVolumeValue(root.uiBridge.volume) <= 0.0001

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

            Repeater {
                model: [
                    { name: "previous", action: root.previousAction },
                    { name: "play", action: root.playAction },
                    { name: "pause", action: root.pauseAction },
                    { name: "stop", action: root.stopAction },
                    { name: "next", action: root.nextAction }
                ]

                delegate: ToolButton {
                    required property var modelData
                    // The action's text without its mnemonic, and its shortcut.
                    readonly property string label: FormatUtils.withoutMnemonic(
                        modelData.action.text || "")
                    readonly property string shortcutText: modelData.action.shortcut
                        ? String(modelData.action.shortcut)
                        : ""

                    objectName: "transportButton_" + modelData.name
                    action: modelData.action
                    display: AbstractButton.IconOnly
                    // Tab reaches it, but a click leaves the focus where it
                    // was, so Space still plays or pauses after a click.
                    focusPolicy: Qt.TabFocus
                    Accessible.name: label
                    ToolTip.visible: hovered || visualFocus
                    ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
                    ToolTip.text: shortcutText.length > 0
                        ? label + " (" + shortcutText + ")"
                        : label
                    // Space presses the focused button rather than playing
                    // or pausing; keys with modifiers stay the shortcuts'.
                    Keys.onShortcutOverride: function(event) {
                        event.accepted = KeyUtils.isPlainSpace(event)
                    }
                }
            }
        }

        Slider {
            id: seekSlider
            objectName: "seekSlider"
            // A drag leaves the focus where it was, so the arrow keys keep
            // working there; Tab reaches the slider for keyboard seeking.
            focusPolicy: Qt.TabFocus
            Layout.fillWidth: true
            from: 0
            to: Math.max(root.uiBridge.durationSeconds, 1.0)
            readonly property bool durationKnown: root.uiBridge.durationSeconds > 1.0
            readonly property bool seekAllowed: durationKnown
                && root.uiBridge.playbackState !== "Stopped"
            readonly property real stableVisualPosition: seekAllowed ? visualPosition : 0.0

            enabled: seekAllowed
            // The arrow keys' step; a drag does not snap to it.
            stepSize: root.keyboardSeekStepSeconds
            snapMode: Slider.NoSnap

            Accessible.name: "Seek"
            Accessible.description: seekAllowed
                ? FormatUtils.formatSeekTime(value) + " of "
                    + FormatUtils.formatSeekTime(root.uiBridge.durationSeconds)
                : "No track playing"

            onPressedChanged: {
                if (!pressed && seekAllowed) {
                    root.seekCommitted(value)
                }
            }
            // The arrow keys move the slider without pressing it, so their
            // moves seek at once.
            onMoved: {
                if (!pressed && seekAllowed) {
                    root.seekCommitted(value)
                }
            }

            background: Item {
                implicitHeight: root.uiPalette.rowHeight
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    objectName: "seekBackground"
                    anchors.fill: parent
                    color: root.uiPalette.uiSurfaceRaisedColor
                    border.color: root.uiPalette.uiBorderColor
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
                    barColor: root.uiPalette.uiWaveformColor
                    bgColor: root.uiPalette.uiSurfaceRaisedColor
                }

                Rectangle {
                    objectName: "seekProgress"
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    visible: seekSlider.seekAllowed
                    width: parent.width * seekSlider.stableVisualPosition
                    color: ColorUtils.withAlpha(root.uiPalette.uiAccentColor, 0.26)
                }

                Rectangle {
                    objectName: "seekPlayhead"
                    visible: seekSlider.seekAllowed
                    width: 1
                    antialiasing: true
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: seekSlider.stableVisualPosition * (parent.width - 1)
                    color: root.uiPalette.uiAccentColor
                }
            }

            handle: Rectangle {
                objectName: "seekHandle"
                visible: seekSlider.seekAllowed
                x: seekSlider.leftPadding + seekSlider.stableVisualPosition
                    * (seekSlider.availableWidth - width)
                y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                implicitWidth: 3
                implicitHeight: seekSlider.height - 4
                radius: 1
                color: root.uiPalette.uiAccentColor
                border.color: ColorUtils.mixColor(
                    root.uiPalette.uiAccentColor, root.uiPalette.uiTextColor, 0.3)
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
                height: Math.max(dragTimeLabel.implicitHeight, seekSlider.availableHeight - 4)
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
                    color: ColorUtils.withAlpha(root.uiPalette.uiAccentColor, 0.85)
                    border.color: ColorUtils.withAlpha(root.uiPalette.uiAccentTextColor, 0.5)

                    Label {
                        id: dragTimeLabel
                        anchors.centerIn: parent
                        text: FormatUtils.formatSeekTime(seekSlider.value)
                        color: root.uiPalette.uiAccentTextColor
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
            id: timeLabel
            text: root.positionText + "/" + root.uiBridge.durationText
            horizontalAlignment: Text.AlignHCenter
            Layout.preferredWidth: Math.ceil(timeTextMetrics.advanceWidth)
            Layout.alignment: Qt.AlignVCenter

            TextMetrics {
                id: timeTextMetrics
                font: timeLabel.font
                text: "00:00:00/00:00:00"
            }
        }

        ToolButton {
            objectName: "muteButton"
            Layout.alignment: Qt.AlignVCenter
            display: AbstractButton.IconOnly
            flat: true
            text: root.volumeMuted ? "Unmute" : "Mute"
            icon.name: root.volumeSilent ? "audio-volume-muted" : "audio-volume-high"
            icon.color: root.uiPalette.uiTextColor
            ToolTip.visible: hovered || visualFocus
            ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
            ToolTip.text: text
            focusPolicy: Qt.TabFocus
            Keys.onShortcutOverride: function(event) {
                event.accepted = KeyUtils.isPlainSpace(event)
            }
            onClicked: root.toggleMutedVolume()
        }

        Slider {
            id: volumeSlider
            objectName: "volumeSlider"
            focusPolicy: Qt.TabFocus
            Layout.preferredWidth: root.uiPalette.rowHeight * 6
            from: 0
            to: 1
            stepSize: 0
            Accessible.name: "Volume"
            Accessible.description: Math.round(value * 100) + "%"
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
