// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils

// The transport (#373): Previous, Play/Pause, Stop and Next as flat 32 px
// tool buttons, the seek waveform, the time and the volume, on the chrome.
// The glyphs and buttons grow with a font larger than the default.
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
    // The playback clock's text of the position, which changes once a
    // second.
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
    readonly property bool playing: root.uiBridge.playbackState === "Playing"
    // The narrow window's transport: a shorter volume slider.
    readonly property bool narrow: root.width < 1200

    // The seek bar's colours. The waveform is `wave`, a little quieter on
    // the chrome; the unplayed part is the same at 50% (dark) or 62%
    // (light), which is at least 3:1 on the chrome, as measured for Breeze.
    readonly property bool themeIsDark: root.uiPalette.themeIsDark
    readonly property color seekPlayedColor: root.themeIsDark
        ? ColorUtils.mixColor(root.uiPalette.wave, root.uiPalette.chrome, 0.12)
        : ColorUtils.mixColor(root.uiPalette.wave, root.uiPalette.text, 0.2)
    // Raised where a theme's colours need more to keep 3:1.
    readonly property real seekUnplayedOpacity: ColorUtils.opacityForContrast(
        root.seekPlayedColor, root.uiPalette.chrome, root.themeIsDark ? 0.5 : 0.62, 3.0)
    // The chrome over the unplayed part, which leaves the waveform at that
    // opacity on the chrome.
    readonly property color seekUnplayedCover: ColorUtils.withAlpha(
        root.uiPalette.chrome, 1 - root.seekUnplayedOpacity)
    // The playhead: the amber of `now`, cased by a line of the opposite
    // brightness on each side, so it stands apart from the played part by
    // its brightness and not only by its hue.
    readonly property color seekPlayheadColor: root.uiPalette.now
    readonly property color seekPlayheadCasing: root.themeIsDark
        ? Qt.rgba(0, 0, 0, 0.6)
        : Qt.rgba(1, 1, 1, 0.85)
    readonly property color seekHoverLineColor: ColorUtils.withAlpha(root.uiPalette.text, 0.7)
    readonly property color seekEmptyLineColor: ColorUtils.withAlpha(root.uiPalette.text, 0.18)
    // The volume slider's groove.
    readonly property color volumeFilledColor: ColorUtils.withAlpha(root.uiPalette.text, 0.65)
    readonly property color volumeUnfilledColor: ColorUtils.withAlpha(root.uiPalette.text, 0.25)

    // The window's device pixels to a logical one, which fractional scaling
    // can make differ from its screen's. Qt before 6.11 gives only the
    // screen's.
    readonly property real windowDevicePixelRatio: Window.window
        && Window.window.devicePixelRatio !== undefined
        ? Window.window.devicePixelRatio
        : Screen.devicePixelRatio

    // Whether the seek bar has a waveform to draw: peaks of the playing
    // track, which the bridge sends as bytes.
    readonly property bool hasWaveform: {
        if (root.uiBridge.playbackState === "Stopped") {
            return false
        }
        const peaks = root.uiBridge.waveformPeaksPacked
        return !!peaks && (peaks.byteLength || peaks.length || 0) > 0
    }
    // A 1 px line, a whole number of device pixels thick.
    readonly property real devicePixelLine: Math.max(1, Math.round(root.windowDevicePixelRatio))
        / root.windowDevicePixelRatio

    // `value` on whole device pixels.
    function snapped(value) {
        return Math.round(value * root.windowDevicePixelRatio) / root.windowDevicePixelRatio
    }

    // The design's glyph and button sizes are for the default 10 pt font; a
    // larger font scales them, and a smaller one keeps them.
    readonly property real fontScale: Math.max(1, root.uiPalette.basePointSize / 10)
    function scaled(size) {
        return Math.max(size, Math.round(size * root.fontScale))
    }
    // The tool buttons: 32 px, or the Play/Pause glyph and 6 px around it.
    readonly property real buttonSize: Math.max(32, root.scaled(20) + 12)

    // A text of the transport's tooltips: the action and its shortcut.
    function actionToolTip(label, shortcutText) {
        return shortcutText.length > 0
            ? I18n.i18nc("@info:tooltip action, shortcut", "%1 · %2", label, shortcutText)
            : label
    }

    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    // 48 px, or the buttons and 8 px above and below them.
    implicitHeight: Math.max(48, root.buttonSize + 16)
    // The content's inset, on both sides: Previous starts where the
    // library's rows and the status bar's text do.
    readonly property int edgeInset: 12
    leftPadding: root.edgeInset
    rightPadding: root.edgeInset
    topPadding: 0
    bottomPadding: 0
    // Chrome. The seam under it is the main window's.
    background: Rectangle {
        color: root.uiPalette.chrome
    }

    // A slider whose held arrow keys are one gesture, as a drag is. Qt's
    // autorepeat clears `pressed` at each repeat, but not `adjusting`.
    component GestureSlider: Slider {
        // The arrow keys held down.
        property var heldKeys: []
        readonly property bool adjusting: gestureHeld()
        readonly property bool pointerHeld: pointerHold.active

        // The mouse's or a touch's hold. Its passive grab leaves the drag
        // to the slider.
        PointHandler {
            id: pointerHold
        }

        // Whether a drag or held keys move the slider, from their own state.
        // A binding that also reads what `adjusting` decides can update
        // between the two, and see `adjusting` still unchanged.
        function gestureHeld() {
            return pressed || heldKeys.length > 0
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

    // The background of the transport's tool buttons, in the window's
    // tokens rather than the style's: the hover tint, which fades, and
    // text 10% while pressed. Keyboard focus is one 2 px ring 3 px outside
    // the button, in the focus colour of the seek bar's and the volume's
    // rings, rather than the style's frame, which is 2.18:1 on the light
    // chrome.
    component ButtonBackground: Item {
        id: buttonBackground

        // The button it is the background of.
        required property Item button

        Rectangle {
            objectName: "transportButtonHover"
            anchors.fill: parent
            radius: 3
            color: buttonBackground.button.down
                ? ColorUtils.withAlpha(root.uiPalette.text, 0.10)
                : root.uiPalette.hover
            opacity: buttonBackground.button.hovered || buttonBackground.button.down ? 1 : 0
            visible: opacity > 0

            Behavior on opacity {
                enabled: Kirigami.Units.shortDuration > 0
                NumberAnimation {
                    duration: Kirigami.Units.shortDuration
                    easing.type: Easing.OutCubic
                }
            }
        }

        Rectangle {
            objectName: "transportButtonFocusRing"
            visible: buttonBackground.button.visualFocus
            anchors.fill: parent
            anchors.margins: -3
            radius: 6
            color: "transparent"
            border.width: 2
            border.color: root.uiPalette.focusRing
        }
    }

    // A flat 32 px tool button of the transport with a theme icon. Its
    // tooltip names the action and its shortcut.
    component TransportButton: ToolButton {
        id: transportButton

        required property string name
        // The glyph's size: 16 px, 20 px for Play/Pause, 14 px for Stop,
        // larger with a larger font.
        property int glyphSize: root.scaled(Kirigami.Units.iconSizes.small)
        // The action's text without its mnemonic, and its shortcut.
        readonly property string label: FormatUtils.withoutMnemonic(
            transportButton.action ? transportButton.action.text || "" : "")
        readonly property string shortcutText: transportButton.action && transportButton.action.shortcut
            ? String(transportButton.action.shortcut)
            : ""
        readonly property color glyphColor: transportButton.enabled
            ? root.uiPalette.text
            : Kirigami.Theme.disabledTextColor

        objectName: "transportButton_" + name
        implicitWidth: root.buttonSize
        implicitHeight: root.buttonSize
        padding: 0
        flat: true
        display: AbstractButton.IconOnly
        icon.width: transportButton.glyphSize
        icon.height: transportButton.glyphSize
        // Tab reaches it, but a click leaves the focus where it was, so
        // Space still plays or pauses after a click.
        focusPolicy: Qt.TabFocus
        Accessible.name: transportButton.label
        ToolTip.visible: hovered || visualFocus
        ToolTip.delay: Kirigami.Units.toolTipDelay
        ToolTip.text: root.actionToolTip(transportButton.label, transportButton.shortcutText)
        // Space presses the focused button rather than playing or pausing;
        // keys with modifiers stay the shortcuts'.
        Keys.onShortcutOverride: function(event) {
            event.accepted = KeyUtils.isPlainSpace(event)
        }

        background: ButtonBackground {
            button: transportButton
        }

        contentItem: Item {
            implicitWidth: transportButton.glyphSize
            implicitHeight: transportButton.glyphSize

            Kirigami.Icon {
                objectName: "transportButtonGlyph"
                anchors.centerIn: parent
                width: transportButton.glyphSize
                height: transportButton.glyphSize
                source: transportButton.icon.name
                color: transportButton.glyphColor
                isMask: true
            }
        }
    }

    contentItem: RowLayout {
        spacing: 0

        Row {
            Layout.alignment: Qt.AlignVCenter
            spacing: Kirigami.Units.smallSpacing

            TransportButton {
                name: "previous"
                action: root.previousAction
            }

            // One button that shows the state: Pause while playing,
            // otherwise Play. Its two glyphs cross-fade over shortDuration,
            // the one shown going out as the other comes in, so the button
            // never goes blank (#373 H4: a sequential swap left it below
            // half its ink for 50 ms). Both opacities come from one
            // progress, so a change back mid-fade reverses from where it
            // is, and at rest one glyph is whole and the other gone.
            TransportButton {
                id: playPauseButton
                name: "playPause"
                glyphSize: root.scaled(20)
                action: root.playing ? root.pauseAction : root.playAction

                contentItem: Item {
                    id: playPauseGlyphs
                    implicitWidth: playPauseButton.glyphSize
                    implicitHeight: playPauseButton.glyphSize

                    // 1 shows Pause, 0 Play: set from the state as it is
                    // first, then by follow().
                    property real pauseProgress: 0
                    readonly property NumberAnimation swap: NumberAnimation {
                        objectName: "transportPlayPauseSwap"
                        target: playPauseGlyphs
                        property: "pauseProgress"
                        duration: Kirigami.Units.shortDuration
                        easing.type: Easing.OutCubic
                    }

                    function follow() {
                        const target = root.playing ? 1 : 0
                        playPauseGlyphs.swap.stop()
                        // Kirigami's durations are 0 with animations off.
                        if (Kirigami.Units.shortDuration <= 0) {
                            playPauseGlyphs.pauseProgress = target
                        } else if (playPauseGlyphs.pauseProgress !== target) {
                            playPauseGlyphs.swap.to = target
                            playPauseGlyphs.swap.start()
                        }
                    }

                    Component.onCompleted: playPauseGlyphs.pauseProgress = root.playing ? 1 : 0

                    Connections {
                        target: root
                        function onPlayingChanged() { playPauseGlyphs.follow() }
                    }

                    Kirigami.Icon {
                        objectName: "transportPlayPauseGlyph_play"
                        anchors.centerIn: parent
                        width: playPauseButton.glyphSize
                        height: playPauseButton.glyphSize
                        source: root.playAction ? root.playAction.icon.name : ""
                        color: playPauseButton.glyphColor
                        isMask: true
                        opacity: 1 - playPauseGlyphs.pauseProgress
                        visible: opacity > 0
                    }

                    Kirigami.Icon {
                        objectName: "transportPlayPauseGlyph_pause"
                        anchors.centerIn: parent
                        width: playPauseButton.glyphSize
                        height: playPauseButton.glyphSize
                        source: root.pauseAction ? root.pauseAction.icon.name : ""
                        color: playPauseButton.glyphColor
                        isMask: true
                        opacity: playPauseGlyphs.pauseProgress
                        visible: opacity > 0
                    }
                }
            }

            TransportButton {
                name: "stop"
                glyphSize: root.scaled(14)
                action: root.stopAction
            }

            TransportButton {
                name: "next"
                action: root.nextAction
            }
        }

        GestureSlider {
            id: seekSlider
            objectName: "seekSlider"
            // A drag leaves the focus where it was, so the arrow keys keep
            // working there; Tab reaches the slider for keyboard seeking.
            focusPolicy: Qt.TabFocus
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: seekSlider.boxHeight
            topPadding: 0
            bottomPadding: 0
            from: 0
            to: durationKnown ? root.uiBridge.durationSeconds : 1.0
            // Any length, however short: a clip of a second or less has one
            // as much as a long track.
            readonly property bool durationKnown: isFinite(root.uiBridge.durationSeconds)
                && root.uiBridge.durationSeconds > 0
            // A track is loaded, playing or paused: the bar shows its
            // playhead, at the start until its length is known.
            readonly property bool trackLoaded: root.uiBridge.playbackState !== "Stopped"
            readonly property bool seekAllowed: durationKnown && trackLoaded
            // The bar: 28 px, with the ±12 px waveform in its middle and
            // the playhead across its height, 2 px past the waveform's
            // peaks on either side.
            readonly property real boxHeight: 28
            readonly property real playheadHeight: boxHeight
            readonly property real waveformHeight: 24
            readonly property real windowDevicePixelRatio: root.windowDevicePixelRatio
            // The whole device pixels the bar holds. Rounded down, so the
            // playhead, which they place, never reaches past the bar's end.
            readonly property real barDevicePixels:
                Math.max(1, Math.floor(width * windowDevicePixelRatio + 1e-6))
            // The playhead is one line, two logical pixels wide, with a
            // casing of one on each side; each is a whole number of device
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
            // frame for a move it would not show. Without a length no time
            // has a place on the bar, so the line waits at the start, where
            // every track begins; the decoded part of the waveform measures
            // the analysis, not playback.
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
                    FormatUtils.formatClockTime(valueSecond),
                    FormatUtils.formatClockTime(root.uiBridge.durationSeconds))
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
                implicitHeight: seekSlider.boxHeight

                // The chrome, as the transport's; the waveform sits on it
                // with no box of its own.
                Rectangle {
                    objectName: "seekBackground"
                    anchors.fill: parent
                    color: root.uiPalette.chrome
                }

                // Without a waveform: a 1 px line through the middle.
                Rectangle {
                    objectName: "seekEmptyLine"
                    visible: !root.hasWaveform
                    antialiasing: false
                    x: 0
                    width: parent.width
                    y: root.snapped((parent.height - height) / 2)
                    height: root.devicePixelLine
                    color: root.seekEmptyLineColor
                }

                WaveformItem {
                    id: waveformItem
                    objectName: "seekWaveform"
                    x: 0
                    width: parent.width
                    // ±12 px about the middle, on whole device pixels.
                    y: root.snapped((parent.height - seekSlider.waveformHeight) / 2)
                    height: seekSlider.waveformHeight
                    visible: root.uiBridge.playbackState !== "Stopped"
                    peaksData: root.uiBridge.playbackState === "Stopped"
                        ? ""
                        : root.uiBridge.waveformPeaksPacked
                    generatedSeconds: root.uiBridge.waveformCoverageSeconds
                    waveformComplete: root.uiBridge.waveformComplete
                    positionSeconds: root.displayedPositionSeconds
                    durationSeconds: root.uiBridge.durationSeconds
                    barColor: root.seekPlayedColor
                    bgColor: root.uiPalette.chrome
                }

                // The unplayed part: the chrome over the waveform after the
                // playhead's line, which leaves it at the unplayed opacity.
                Rectangle {
                    objectName: "seekUnplayed"
                    visible: seekSlider.trackLoaded
                    antialiasing: false
                    x: seekSlider.playheadX
                    width: Math.max(0, parent.width - x)
                    y: waveformItem.y
                    height: waveformItem.height
                    color: root.seekUnplayedCover
                }

                // The playhead: its casing, cut to the bar at either end,
                // with the line in it.
                Rectangle {
                    objectName: "seekPlayhead"
                    visible: seekSlider.trackLoaded
                    antialiasing: false
                    y: root.snapped((parent.height - seekSlider.playheadHeight) / 2)
                    height: seekSlider.playheadHeight
                    x: Math.max(0, seekSlider.playheadX - seekSlider.playheadOutlineWidth)
                    width: Math.min(parent.width, seekSlider.playheadX + seekSlider.playheadLineWidth
                        + seekSlider.playheadOutlineWidth) - x
                    color: root.seekPlayheadCasing

                    Rectangle {
                        objectName: "seekPlayheadLine"
                        antialiasing: false
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        x: seekSlider.playheadX - parent.x
                        width: seekSlider.playheadLineWidth
                        color: root.seekPlayheadColor
                    }
                }

                // A 1 px line at the hovering pointer, which comes and goes
                // with the time box over it: at the box's opacity while the
                // box shows, or fades out, the hover's time, where that last
                // was. A gesture shows its time at the playhead with no line.
                Rectangle {
                    objectName: "seekHoverLine"
                    opacity: seekTimeBox.showsHover && seekTimeBox.contentItem.visible ? seekTimeBox.opacity : 0
                    visible: opacity > 0
                    antialiasing: false
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: Math.max(0, Math.min(parent.width - width,
                        Math.floor(seekSlider.hoverX * seekSlider.windowDevicePixelRatio)
                            / seekSlider.windowDevicePixelRatio))
                    width: root.devicePixelLine
                    color: root.seekHoverLineColor
                }

                // Keyboard focus: one 2 px ring 3 px around the bar.
                Rectangle {
                    objectName: "seekFocusRing"
                    visible: seekSlider.visualFocus
                    anchors.fill: parent
                    anchors.margins: -5
                    radius: 5
                    color: "transparent"
                    border.width: 2
                    border.color: root.uiPalette.focusRing
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
                // From the bar's height rather than the slider's, which
                // depends on the handle's.
                implicitHeight: seekSlider.boxHeight - 4
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
            // Where the hovering pointer is, in the slider's coordinates, and
            // the second a click there would seek to, as a drag there shows
            // it. Hidden, they keep their last values, so the tooltip fades
            // out where it was, with its time, and nothing hidden moves. Qt
            // sends no hover during a drag, and brings the point up to date
            // at the frame after it, before that frame draws.
            property real hoverX: 0
            property int hoverSecond: 0
            Binding on hoverX {
                when: seekSlider.hoverTimeShown
                value: seekHover.point.position.x
                restoreMode: Binding.RestoreNone
            }
            Binding on hoverSecond {
                when: seekSlider.hoverTimeShown
                // Without a length, whose stand-in range puts the start at
                // every point, the last second stays. From the bridge, as
                // `durationKnown` and `hoverTimeShown` can still hold the
                // last length's values when this updates.
                value: isFinite(root.uiBridge.durationSeconds) && root.uiBridge.durationSeconds > 0
                    ? Math.floor(seekSlider.secondsAtX(seekHover.point.position.x))
                    : seekSlider.hoverSecond
                restoreMode: Binding.RestoreNone
            }

            // The time in a native tooltip under the bar: at the playhead
            // while a gesture moves it, otherwise at the hovering pointer.
            ToolTip {
                id: seekTimeBox
                objectName: "seekTimeBox"
                parent: seekSlider
                // It follows the pointer and the gesture, not clicks or keys.
                closePolicy: Popup.NoAutoClose
                readonly property bool shown: (seekSlider.adjusting || seekSlider.hoverTimeShown)
                    && seekSlider.seekAllowed
                // Whether the time it shows, or last showed, is the hover's
                // rather than a gesture's: the hover line then shows, and
                // fades out, with it.
                property bool showsHover: false
                Binding on showsHover {
                    when: seekTimeBox.shown
                    value: !seekSlider.gestureHeld()
                    restoreMode: Binding.RestoreNone
                }
                visible: shown
                delay: 0
                timeout: -1
                // The point the tooltip is centred on: the playhead's during
                // a gesture, the pointer's while it hovers, and otherwise
                // where it last was, so the tooltip fades out there and
                // playback moves no hidden tooltip. From the gesture's and
                // the hover's own state, as `shown` can still hold its last
                // value when this updates.
                property real anchorX: 0
                Binding on anchorX {
                    value: seekSlider.gestureHeld()
                        ? seekSlider.playheadX + seekSlider.playheadLineWidth / 2
                        : seekHover.hovered ? seekSlider.hoverX : seekTimeBox.anchorX
                    restoreMode: Binding.RestoreNone
                }
                // On whole device pixels, so a pointer's move within one
                // moves the tooltip by none and draws no frame.
                readonly property real placedX: root.snapped(anchorX - width / 2)
                x: placedX
                y: seekSlider.height + Kirigami.Units.smallSpacing
                // The time while shown, and nothing while hidden, so that
                // playback's seconds change no hidden text, which would draw
                // frames. The text keeps the last time while it fades out.
                // Its source is chosen as the anchor's is.
                readonly property string shownText: !shown ? ""
                    : seekSlider.gestureHeld() ? FormatUtils.formatClockTime(seekSlider.valueSecond)
                    : seekHover.hovered ? FormatUtils.formatClockTime(seekSlider.hoverSecond)
                    : ""
                onShownTextChanged: {
                    if (shownText.length > 0) {
                        text = shownText
                    }
                }

                contentItem: Label {
                    objectName: "seekDragTime"
                    text: seekTimeBox.text
                    font.family: seekTimeBox.font.family
                    font.pointSize: seekTimeBox.font.pointSize
                    font.features: root.uiPalette.tabularFigures
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

        // "1:17 / 3:49": the position in the text colour, the length in the
        // secondary one; "–:––" with nothing playing.
        Item {
            id: transportTime
            objectName: "transportTime"
            readonly property bool empty: root.uiBridge.playbackState === "Stopped"
            readonly property string lengthText: seekSlider.durationKnown
                ? FormatUtils.clockText(root.uiBridge.durationText)
                : I18n.i18nc("@info:status no time", "–:––")
            readonly property string text: timePosition.text + timeLength.text

            Layout.leftMargin: 16
            Layout.alignment: Qt.AlignVCenter
            // 72 px, or what a time in hours needs. The position is never
            // wider than the length.
            Layout.preferredWidth: Math.max(72, Math.ceil(timeMetrics.advanceWidth))
            Layout.preferredHeight: timeRow.implicitHeight

            TextMetrics {
                id: timeMetrics
                font: timePosition.font
                text: transportTime.lengthText + timeLength.text
            }

            Row {
                id: timeRow
                anchors.verticalCenter: parent.verticalCenter

                Label {
                    id: timePosition
                    objectName: "transportTimePosition"
                    font.features: root.uiPalette.tabularFigures
                    color: transportTime.empty ? root.uiPalette.textSecondary : root.uiPalette.text
                    text: transportTime.empty
                        ? I18n.i18nc("@info:status no time", "–:––")
                        : FormatUtils.clockText(root.positionText)
                }

                Label {
                    id: timeLength
                    objectName: "transportTimeLength"
                    visible: !transportTime.empty
                    font.features: root.uiPalette.tabularFigures
                    color: root.uiPalette.textSecondary
                    text: transportTime.empty ? ""
                        : I18n.i18nc("@info:status the track's length after its position", " / %1",
                            transportTime.lengthText)
                }
            }
        }

        ToolButton {
            id: muteButton
            objectName: "muteButton"
            Layout.leftMargin: 16
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: root.buttonSize
            implicitHeight: root.buttonSize
            padding: 0
            display: AbstractButton.IconOnly
            flat: true
            text: root.volumeMuted
                ? I18n.i18nc("@action:button", "Unmute")
                : I18n.i18nc("@action:button", "Mute")
            icon.name: root.volumeSilent ? "audio-volume-muted" : "audio-volume-high"
            icon.color: root.uiPalette.text
            icon.width: root.scaled(Kirigami.Units.iconSizes.small)
            icon.height: root.scaled(Kirigami.Units.iconSizes.small)
            ToolTip.visible: hovered || visualFocus
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: text
            focusPolicy: Qt.TabFocus
            Keys.onShortcutOverride: function(event) {
                event.accepted = KeyUtils.isPlainSpace(event)
            }
            // The style drew the glyph with its frame; this background
            // draws neither.
            contentItem: Item {
                implicitWidth: muteButton.icon.width
                implicitHeight: muteButton.icon.height

                Kirigami.Icon {
                    objectName: "muteButtonGlyph"
                    anchors.centerIn: parent
                    width: muteButton.icon.width
                    height: muteButton.icon.height
                    source: muteButton.icon.name
                    color: muteButton.icon.color
                    isMask: true
                }
            }
            background: ButtonBackground {
                button: muteButton
            }
            onClicked: root.toggleMutedVolume()
        }

        GestureSlider {
            id: volumeSlider
            objectName: "volumeSlider"
            focusPolicy: Qt.TabFocus
            Layout.leftMargin: Kirigami.Units.smallSpacing
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: root.narrow ? 72 : 96
            Layout.preferredHeight: 20
            padding: 0
            from: 0
            to: 1
            stepSize: 0
            hoverEnabled: true
            readonly property string percentText: I18n.i18nc("@info volume in percent", "%1%",
                Math.round(value * 100))
            Accessible.name: I18n.i18nc("@label", "Volume")
            Accessible.description: volumeSlider.percentText
            ToolTip.visible: hovered || visualFocus || pressed
            ToolTip.delay: pressed ? 0 : Kirigami.Units.toolTipDelay
            ToolTip.text: I18n.i18nc("@info:tooltip volume in percent", "Volume %1",
                volumeSlider.percentText)
            // Every move, including a release's, sends its value, so the
            // volume follows a drag or a held key.
            onMoved: root.setAppVolume(value)

            // A 2 px groove: filled up to the thumb at text 65%, the rest at
            // text 25%.
            background: Item {
                implicitWidth: 96
                implicitHeight: 20

                Rectangle {
                    objectName: "volumeGroove"
                    x: 0
                    width: parent.width
                    y: root.snapped((parent.height - height) / 2)
                    height: 2
                    radius: 1
                    color: root.volumeUnfilledColor
                }

                Rectangle {
                    objectName: "volumeGrooveFill"
                    x: 0
                    width: volumeSlider.handle
                        ? volumeSlider.handle.x + volumeSlider.handle.width / 2
                        : 0
                    y: root.snapped((parent.height - height) / 2)
                    height: 2
                    radius: 1
                    color: root.volumeFilledColor
                }
            }

            // A 14 px thumb. Hover tints it with the accent; keyboard focus
            // draws a 2 px ring around it.
            handle: Rectangle {
                objectName: "volumeThumb"
                readonly property bool tinted: volumeSlider.hovered || volumeSlider.pressed
                x: volumeSlider.leftPadding + volumeSlider.visualPosition
                    * (volumeSlider.availableWidth - width)
                y: volumeSlider.topPadding + (volumeSlider.availableHeight - height) / 2
                implicitWidth: 14
                implicitHeight: 14
                radius: width / 2
                color: tinted
                    ? ColorUtils.mixColor(volumeSlider.palette.button, root.uiPalette.accent, 0.2)
                    : volumeSlider.palette.button
                border.width: 1
                border.color: tinted
                    ? root.uiPalette.accent
                    : ColorUtils.withAlpha(root.uiPalette.text, 0.5)

                Behavior on color {
                    enabled: Kirigami.Units.shortDuration > 0
                    ColorAnimation {
                        duration: Kirigami.Units.shortDuration
                        easing.type: Easing.OutCubic
                    }
                }

                Rectangle {
                    objectName: "volumeFocusRing"
                    visible: volumeSlider.visualFocus
                    anchors.fill: parent
                    anchors.margins: -3
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: root.uiPalette.focusRing
                }
            }
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
