// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
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
    // The position as shown, which moves by positionStepSeconds.
    required property real displayedPositionSeconds
    // The position now, for a step of the keys or the wheel.
    property var currentPositionSeconds: function() { return root.displayedPositionSeconds }
    property string positionText: "00:00"
    // The whole second positionText shows, which changes every second
    // however few pixels the position moves by.
    property int positionSecond: Math.floor(root.displayedPositionSeconds + 0.5)
    required property var toggleMutedVolume
    required property var setAppVolume
    required property var normalizedVolumeValue
    required property var seekCommitted

    readonly property alias seekPressed: seekSlider.pressed
    // The time of one device pixel of the seek bar's playhead: the smallest
    // move of the position it shows, so the position needs to change no more
    // often.
    readonly property real positionStepSeconds: seekSlider.to / seekSlider.travelDevicePixels
    readonly property bool volumeSilent: root.volumeMuted
        || root.normalizedVolumeValue(root.uiBridge.volume) <= 0.0001

    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
    leftPadding: 8
    rightPadding: 12
    topPadding: 4
    bottomPadding: 4

    // A slider whose held arrow keys are one gesture, as a drag is. Qt's
    // autorepeat clears `pressed` at each repeat, but not `adjusting`.
    component GestureSlider: Slider {
        // The arrow keys held down.
        property var heldKeys: []
        readonly property bool adjusting: pressed || heldKeys.length > 0
        readonly property bool pointerHeld: pointerHold.active

        // The mouse's or a touch's hold. Its passive grab leaves the drag
        // to the slider.
        PointHandler {
            id: pointerHold
        }

        // The slider handles the keys after these, unless they accept them.
        Keys.onPressed: function(event) {
            if ((event.key === Qt.Key_Left || event.key === Qt.Key_Right)
                    && !heldKeys.includes(event.key)) {
                heldKeys = heldKeys.concat([event.key])
            }
        }
        Keys.onReleased: function(event) {
            if (!event.isAutoRepeat && heldKeys.includes(event.key)) {
                heldKeys = heldKeys.filter(key => key !== event.key)
            }
            // The slider clears `pressed` at any key's release, which would
            // end the pointer's drag.
            event.accepted = pointerHold.active
        }
        // The keys' releases go elsewhere then, so the slider would stay
        // pressed by the keys.
        onActiveFocusChanged: {
            if (!activeFocus && heldKeys.length > 0) {
                heldKeys = []
                if (!pointerHold.active) {
                    pressed = false
                }
            }
        }
    }

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
                        ? I18n.i18nc("@info:tooltip action, shortcut", "%1 (%2)", label, shortcutText)
                        : label
                    // Space presses the focused button rather than playing
                    // or pausing; keys with modifiers stay the shortcuts'.
                    Keys.onShortcutOverride: function(event) {
                        event.accepted = KeyUtils.isPlainSpace(event)
                    }
                }
            }
        }

        GestureSlider {
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
            // The window's device pixels to a logical one, which fractional
            // scaling can make differ from its screen's. Qt before 6.11 gives
            // only the screen's.
            readonly property real windowDevicePixelRatio: Window.window
                && Window.window.devicePixelRatio !== undefined
                ? Window.window.devicePixelRatio
                : Screen.devicePixelRatio
            // The whole device pixels the bar holds. Rounded down, so the
            // playhead, which they place, never reaches past the bar's end.
            readonly property real barDevicePixels:
                Math.max(1, Math.floor(width * windowDevicePixelRatio + 1e-6))
            // The playhead is one line, two logical pixels wide, with an
            // outline of one on each side; each is a whole number of device
            // pixels, so the line keeps its width and brightness as it moves.
            readonly property int playheadLineDevicePixels: Math.max(1, Math.round(2 * windowDevicePixelRatio))
            readonly property real playheadLineWidth: playheadLineDevicePixels / windowDevicePixelRatio
            readonly property real playheadOutlineWidth:
                Math.max(1, Math.round(windowDevicePixelRatio)) / windowDevicePixelRatio
            // The device pixels the line's left edge moves over: the bar's,
            // less the line's, so the line stays on the bar at either end.
            readonly property real travelDevicePixels: Math.max(1, barDevicePixels - playheadLineDevicePixels)
            // The left edge of the line on the bar, on whole device pixels,
            // so the line moves only by whole pixels and the window draws no
            // frame for a move it would not show.
            readonly property real playheadX: !seekAllowed ? 0.0
                : Math.round(visualPosition * travelDevicePixels) / windowDevicePixelRatio
            // The handle is a grab target wider than the line and not drawn.
            // Padding of half their difference outside the bar puts the
            // handle's centre, which a press or drag moves under the
            // pointer, on the line's centre at every position.
            readonly property real grabTargetWidth: 10
            leftPadding: (playheadLineWidth - grabTargetWidth) / 2
            rightPadding: (playheadLineWidth - grabTargetWidth) / 2

            enabled: seekAllowed
            // The arrow keys', the wheel's and assistive technology's step.
            // The slider rounds a pointer's value to its step whatever the
            // snap mode, so a press by the pointer alone has none: a drag or
            // a click shows and seeks to the time under the pointer.
            stepSize: pressed && heldKeys.length === 0 ? 0 : root.keyboardSeekStepSeconds
            snapMode: Slider.NoSnap

            // The time at `x`, in the slider's coordinates, where a press
            // there moves the slider; the slider's own mapping, without its
            // step.
            function secondsAtX(x) {
                const handleWidth = handle ? handle.width : 0
                const extent = availableWidth - handleWidth
                if (Math.abs(extent) < 1e-12) {
                    return from
                }
                const offset = mirrored
                    ? width - x - rightPadding - handleWidth / 2
                    : x - leftPadding - handleWidth / 2
                return from + Math.max(0, Math.min(1, offset / extent)) * (to - from)
            }

            // The second the texts show: a gesture's under the pointer or
            // keys, otherwise playback's, which changes every second while
            // the value moves by pixels, which can be seconds apart.
            readonly property int valueSecond: adjusting ? Math.floor(value) : root.positionSecond

            Accessible.name: I18n.i18nc("@label", "Seek")
            Accessible.description: seekAllowed
                ? I18n.i18nc("@info position of duration", "%1 of %2",
                    FormatUtils.formatSeekTime(valueSecond),
                    FormatUtils.formatSeekTime(root.uiBridge.durationSeconds))
                : I18n.i18nc("@info", "No track playing")

            // Where the current gesture moved the slider, or NaN. The
            // position's Binding may write the value first when it ends.
            property real gestureValue: NaN
            // The value before its latest change, which a step of the keys
            // or the wheel moves it from.
            property real valueBeforeChange: 0
            property real latestValue: 0
            onValueChanged: {
                valueBeforeChange = latestValue
                latestValue = value
            }

            // A drag or a held key seeks once, when it ends, so the backend
            // gets no seek per autorepeat; other moves, such as the wheel's,
            // seek at once.
            onMoved: {
                const stepped = !pointerHeld && (heldKeys.length > 0 || !pressed)
                if (stepped && isNaN(gestureValue) && durationKnown && value > from && value < to) {
                    // The first step of the keys or a step of the wheel
                    // moves from the position now, which the value shows
                    // only to its last pixel. A step to either end stays.
                    const target = value - valueBeforeChange + root.currentPositionSeconds()
                    value = Math.max(from, Math.min(to, target))
                }
                if (adjusting) {
                    gestureValue = value
                } else if (seekAllowed) {
                    root.seekCommitted(value)
                }
            }
            onAdjustingChanged: {
                if (!adjusting && !isNaN(gestureValue)) {
                    const target = gestureValue
                    gestureValue = NaN
                    if (seekAllowed) {
                        root.seekCommitted(target)
                    }
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
                    // Up to the playhead's line.
                    width: seekSlider.playheadX
                    color: ColorUtils.withAlpha(root.uiPalette.uiAccentColor, 0.26)
                }

                // The playhead: its outline, cut to the bar at either end,
                // with the line in it. One of the two contrasts with
                // whatever is behind them.
                Rectangle {
                    objectName: "seekPlayhead"
                    visible: seekSlider.seekAllowed
                    antialiasing: false
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: Math.max(0, seekSlider.playheadX - seekSlider.playheadOutlineWidth)
                    width: Math.min(parent.width, seekSlider.playheadX + seekSlider.playheadLineWidth
                        + seekSlider.playheadOutlineWidth) - x
                    color: root.uiPalette.uiPlayheadOutlineColor

                    Rectangle {
                        objectName: "seekPlayheadLine"
                        antialiasing: false
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        x: seekSlider.playheadX - parent.x
                        width: seekSlider.playheadLineWidth
                        color: root.uiPalette.uiPlayheadColor
                    }
                }
            }

            // The grab target, centred on the playhead's line; it draws
            // nothing.
            handle: Item {
                objectName: "seekGrabTarget"
                visible: seekSlider.seekAllowed
                x: seekSlider.playheadX + (seekSlider.playheadLineWidth - width) / 2
                y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                implicitWidth: seekSlider.grabTargetWidth
                // From the background's height rather than the slider's,
                // which depends on the handle's.
                implicitHeight: root.uiPalette.rowHeight - 4
            }

            // The mouse's hover; a touch has none. It only shows the time
            // under the pointer, and never moves the slider or seeks.
            HoverHandler {
                id: seekHover
                objectName: "seekHover"
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            }

            // The time under the hovering pointer, shown while no gesture
            // shows its own.
            readonly property bool hoverTimeShown: seekHover.hovered && !adjusting && seekAllowed
            // Where the hovering pointer is, in the slider's coordinates;
            // unchanging while hidden, so the hidden box does not move. Qt
            // sends no hover during a drag, and brings the point up to date
            // at the frame after it, before that frame draws.
            readonly property real hoverX: hoverTimeShown ? seekHover.point.position.x : 0
            // The second a click at the pointer would seek to, as a drag
            // there shows it.
            readonly property int hoverSecond: hoverTimeShown ? Math.floor(secondsAtX(hoverX)) : 0

            // The time box: beside the playhead while a gesture moves it,
            // otherwise beside the hovering pointer.
            Item {
                id: seekTimeBox
                objectName: "seekTimeBox"
                visible: (seekSlider.adjusting || seekSlider.hoverTimeShown) && seekSlider.seekAllowed
                z: 20
                // The point the box is beside; unchanging while hidden, so
                // playback moves no hidden box.
                property real anchorX: seekSlider.adjusting
                    ? seekSlider.playheadX + seekSlider.playheadLineWidth / 2
                    : seekSlider.hoverX
                property real leftCandidateX: anchorX - width - 8
                property real rightCandidateX: anchorX + 8
                width: dragTimeLabel.implicitWidth + 14
                height: Math.max(dragTimeLabel.implicitHeight, seekSlider.availableHeight - 4)
                y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                // On whole device pixels, so a pointer's move within one
                // moves the box by none and draws no frame.
                x: {
                    const minX = 2
                    const maxX = seekSlider.width - width - 2
                    const boxX = leftCandidateX >= minX
                        ? Math.min(maxX, leftCandidateX)
                        : Math.max(minX, Math.min(maxX, rightCandidateX))
                    const ratio = seekSlider.windowDevicePixelRatio
                    return Math.round(boxX * ratio) / ratio
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 2
                    color: ColorUtils.withAlpha(root.uiPalette.uiAccentColor, 0.85)
                    border.color: ColorUtils.withAlpha(root.uiPalette.uiAccentTextColor, 0.5)

                    Label {
                        id: dragTimeLabel
                        objectName: "seekDragTime"
                        anchors.centerIn: parent
                        // Only while it is shown, so that playback's seconds
                        // change no hidden text, which would draw frames.
                        text: !seekTimeBox.visible ? ""
                            : FormatUtils.formatSeekTime(seekSlider.adjusting
                                ? seekSlider.valueSecond
                                : seekSlider.hoverSecond)
                        color: root.uiPalette.uiAccentTextColor
                    }
                }
            }
        }

        Binding {
            target: seekSlider
            property: "value"
            value: seekSlider.durationKnown ? root.displayedPositionSeconds : 0
            when: !seekSlider.adjusting
            // A gesture keeps the position it starts from; restoring the
            // value from before the binding would make the arrow keys step
            // from it.
            restoreMode: Binding.RestoreNone
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
            text: root.volumeMuted
                ? I18n.i18nc("@action:button", "Unmute")
                : I18n.i18nc("@action:button", "Mute")
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

        GestureSlider {
            id: volumeSlider
            objectName: "volumeSlider"
            focusPolicy: Qt.TabFocus
            Layout.preferredWidth: root.uiPalette.rowHeight * 6
            from: 0
            to: 1
            stepSize: 0
            Accessible.name: I18n.i18nc("@label", "Volume")
            Accessible.description: Math.round(value * 100) + "%"
            // Every move, including a release's, sends its value, so the
            // volume follows a drag or a held key.
            onMoved: root.setAppVolume(value)
        }

        Binding {
            target: volumeSlider
            property: "value"
            value: root.uiBridge.volume
            when: !volumeSlider.adjusting
            // As for the seek slider.
            restoreMode: Binding.RestoreNone
        }
    }
}
