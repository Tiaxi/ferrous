// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/ErrorUtils.js" as ErrorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils

// The transport (#373): Previous, Play/Pause, Stop and Next as flat 32 px
// tool buttons, the seek waveform, the time and the volume, on the chrome.
// The chrome is the theme's Header colour set, as Breeze's toolbars and
// the menu bar above it are, so the two read as one band of window chrome:
// #292c30 / #dee0e2 in Breeze, and the window colour while the window is
// inactive, as Breeze's Header set has it.
// The glyphs and buttons grow with a font larger than the default, and are
// drawn at the size they are given, not rounded down to a theme icon size.
ToolBar {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var previousAction
    required property var playAction
    required property var pauseAction
    required property var stopAction
    required property var nextAction
    // The action Space triggers, whose shortcut the Play/Pause tooltip
    // names: Play and Pause have none of their own.
    property var playPauseAction: null
    // The action whose shortcut Mute's tooltip names.
    property var muteAction: null
    // Whether each button can act now: off, it shows Breeze's disabled
    // state, as the Playback menu's item of its action does, while the
    // action stays on for the shortcuts.
    property bool playPauseCanAct: true
    property bool stopCanAct: true
    property bool previousCanAct: true
    property bool nextCanAct: true
    required property bool volumeMuted
    // The volume a mute put aside, which the slider keeps showing while
    // muted.
    property real mutedVolume: 1.0
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
    // The part of the track the visualization's view shows while it is
    // zoomed, in seconds, or NaN: the seek bar marks it with a band behind
    // its waveform.
    property real viewStartSeconds: NaN
    property real viewEndSeconds: NaN
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

    // The transport's chrome: the Header colour set's background.
    Kirigami.Theme.colorSet: Kirigami.Theme.Header
    Kirigami.Theme.inherit: false
    readonly property color chromeColor: Kirigami.Theme.backgroundColor
    // The hover and pressed tints of its buttons, as the window's, but as
    // strong on this chrome: 1.3:1 and 1.65:1 on it.
    readonly property color hoverTint: root.uiPalette.hoverOn(root.chromeColor)
    readonly property color pressedTint: root.uiPalette.pressedOn(root.chromeColor)

    // The seek bar's colours. The waveform is `wave`, a little quieter on
    // the chrome (dark) or a little darker (light); the unplayed part is the
    // same at 50% (dark) or 62% (light), raised where that is under 3.2:1
    // on the chrome, a margin over 3:1 that rendering cannot round away
    // (unraised, the tones were 2.99:1 / 2.72:1 on the Header colours). In
    // Breeze, played is 8.2:1 / 7.4:1 and unplayed 3.2:1 /
    // 3.3:1 on the active Header colours, about 2.5:1 / 2.3:1 apart.
    readonly property bool themeIsDark: root.uiPalette.themeIsDark
    readonly property color seekPlayedColor: root.themeIsDark
        ? ColorUtils.mixColor(root.uiPalette.wave, root.chromeColor, 0.12)
        : ColorUtils.mixColor(root.uiPalette.wave, root.uiPalette.text, 0.3)
    readonly property real seekUnplayedOpacity: ColorUtils.opacityForContrast(
        root.seekPlayedColor, root.chromeColor, root.themeIsDark ? 0.5 : 0.62, 3.2)
    // The chrome over the unplayed part, which leaves the waveform at that
    // opacity on the chrome.
    readonly property color seekUnplayedCover: ColorUtils.withAlpha(
        root.chromeColor, 1 - root.seekUnplayedOpacity)
    // The playhead: the amber of `now`, cased by a line of the opposite
    // brightness on each side, so it stands apart from the played part by
    // its brightness and not only by its hue.
    readonly property color seekPlayheadColor: root.uiPalette.now
    readonly property color seekPlayheadCasing: root.themeIsDark
        ? Qt.rgba(0, 0, 0, 0.6)
        : Qt.rgba(1, 1, 1, 0.85)
    readonly property color seekHoverLineColor: ColorUtils.withAlpha(root.uiPalette.text, 0.7)
    readonly property color seekEmptyLineColor: ColorUtils.withAlpha(root.uiPalette.text, 0.18)
    // The band of the visualization's view: the view colour, of the panes
    // the window shows its content in, as a quiet well in the chrome (about
    // 1.28:1 on Breeze Dark's Header colour). It is darker than the dark
    // chrome and lighter than the light one, away from the waveform's
    // tones, so the waveform on it keeps its contrast. On the light theme
    // it is 45 % of the way to the chrome, about 1.17:1 on Breeze's Header
    // colour: the view's white, 1.32:1, and a quarter of the way, 1.23:1,
    // read as a raised white slot, louder than the dark band's well (H16,
    // H17). The unplayed part is the waveform at the unplayed opacity on
    // the band as on the chrome. The played part is under the band's
    // colour at 8 %, 6 % on the light theme's pale band, so the band's edge
    // shows in the waveform's ink on either side of the playhead: under the
    // bright played part alone, the band read as starting at the playhead
    // rather than centred on it. More paled the played part to a tone of
    // its own, a step inside the band (H15, H17 light), and in dark left
    // the part in view the dimmest of the bar (H17).
    readonly property color seekViewBandColor: root.themeIsDark
        ? root.uiPalette.view
        : ColorUtils.mixColor(root.uiPalette.view, root.chromeColor, 0.45)
    readonly property color seekViewBandUnplayedCover: ColorUtils.withAlpha(
        root.seekViewBandColor, 1 - root.seekUnplayedOpacity)
    readonly property color seekViewBandPlayedCover: ColorUtils.withAlpha(
        root.seekViewBandColor, root.themeIsDark ? 0.08 : 0.06)
    // The volume slider, in the seek bar's language: its groove filled up
    // to the thumb in the played colour and the rest in the unplayed one,
    // opaque, and a solid thumb in the played colour. While muted, the
    // filled part and the thumb at rest dim to the unplayed
    // colour together, and the thumb stays at the volume a mute put aside.
    readonly property color volumeFilledColor: root.seekPlayedColor
    readonly property color volumeUnfilledColor: ColorUtils.mixColor(
        root.chromeColor, root.seekPlayedColor, root.seekUnplayedOpacity)
    // The thumb under the pointer or dragged, a clear step from it at
    // rest: on dark the text colour, brighter than the played colour
    // (#fcfcfc from #a8cbde); on light the accent as the focus ring draws
    // it, as Breeze marks a hovered handle, 3:1 on the chrome (#1c76a6 from
    // #294758). The light theme's text, #232629, was all but the played
    // colour, so hover and a drag went unseen.
    readonly property color volumeThumbActiveColor: root.themeIsDark
        ? root.uiPalette.text
        : root.uiPalette.focusRing

    // The window's device pixels to a logical one, which fractional scaling
    // can make differ from its screen's. Qt before 6.11 gives only the
    // screen's.
    readonly property real windowDevicePixelRatio: Window.window
        && Window.window.devicePixelRatio !== undefined
        ? Window.window.devicePixelRatio
        : Screen.devicePixelRatio

    readonly property bool stopped: root.uiBridge.playbackState === "Stopped"
    // Stopped with a track loaded, which is not the empty transport (it
    // was drawn as one): the bar has the track's waveform, all unplayed and
    // with no playhead, and the time its length. The engine keeps a stopped
    // track current until the queue is empty.
    readonly property bool stoppedTrack: root.stopped && root.uiBridge.queueLength > 0
        && (root.uiBridge.currentTrackPath || "").length > 0

    // Whether the seek bar has a waveform to draw: peaks of the current
    // track, which the bridge sends as bytes. Stopped, only what it holds
    // of that track: the bridge keeps the peaks of the track before until
    // the next one's come, and covers none of it until then.
    readonly property bool hasWaveform: {
        if (root.stopped && !(root.stoppedTrack
                && (root.uiBridge.waveformCoverageSeconds > 0 || root.uiBridge.waveformComplete))) {
            return false
        }
        const peaks = root.uiBridge.waveformPeaksPacked
        return !!peaks && (peaks.byteLength || peaks.length || 0) > 0
    }
    // Where the file of the current track is known to end early, in
    // seconds, or -1: the bridge tells it of the current track alone, and
    // the bar has a place for it with that track's waveform and length.
    readonly property real endsEarlySeconds: root.hasWaveform
        && root.uiBridge.waveformEndsEarly === true && root.uiBridge.durationSeconds > 0
        ? root.uiBridge.waveformCoverageSeconds : -1
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
    // The transport's two glyph sizes (Play/Pause 22, Previous and Next 16
    // and Stop 14 read as three weights): 20 px for Play/Pause, a
    // step above the 16 px of the others, the volume's included.
    readonly property int playPauseGlyphSize: root.scaled(20)
    readonly property int smallGlyphSize: root.scaled(Kirigami.Units.iconSizes.small)
    // The tool buttons: 32 px, or the Play/Pause glyph and 6 px around it.
    readonly property real buttonSize: Math.max(32, root.playPauseGlyphSize + 12)
    // The clear space around a 16 px glyph in its button.
    readonly property real smallGlyphInset: (root.buttonSize - root.smallGlyphSize) / 2
    // The right of the transport on one rhythm: the seek bar, 16 px, the
    // time, 24 px, the volume glyph, measured between what is drawn, not
    // between the buttons' frames; and the volume's groove 6 px from the
    // mute button's frame, 3 px clear of its focus ring (8 px from the
    // glyph, the groove met the hover's fill and went through the ring).
    readonly property real seekToTimeGap: 2 * Kirigami.Units.largeSpacing
    readonly property real timeToVolumeGap: 3 * Kirigami.Units.largeSpacing
    readonly property real muteButtonToSliderGap: 2 * root.buttonFocusRingOutset

    // A text of the transport's tooltips: the action and its shortcut.
    function actionToolTip(label, shortcutText) {
        return shortcutText.length > 0
            ? I18n.i18nc("@info:tooltip action, shortcut", "%1 · %2", label, shortcutText)
            : label
    }

    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    // The row of controls: 48 px, or the buttons and 8 px above and below
    // them, or the seek bar's focus ring and 3 px above and below it, so
    // that at a large font the ring stays 3 px clear of the seam and the
    // row is the buttons' (76 px at 24 pt; a 5 px ring with 5 px under it
    // made it 82 px).
    readonly property real rowHeight: Math.max(48, root.buttonSize + 16,
        seekSlider.boxHeight + 2 * seekSlider.focusRingOutset + 6)
    // The transport's popups have one place: a lane over the row, in the
    // menu bar's row, borrowed while a transport popup is open. The seek
    // bar's time box, the buttons' tooltips, Mute's and the volume's open
    // there, so the band has one rail (the buttons' opened under the band,
    // over the cover and the queue's header, and Mute's and the volume's
    // above it). Under the bar the time box hung over the queue's header
    // (H5) and, at a large font, the now-playing title; beside its line on
    // the waveform it hid what the scrub reads; a lane of the band's own,
    // empty at rest, made the band 80 px tall for its 48 px row (158 px at
    // 24 pt). The lane ends laneRingGap clear of the seek bar's focus ring
    // and of the buttons', and its boxes are a line of text and a little
    // room above and below it. The lane's bottom, up from the row's centre
    // line:
    readonly property real laneClearance: Math.max(
        seekSlider.boxHeight / 2 + seekSlider.focusRingOutset + root.laneRingGap,
        root.buttonSize / 2 + root.buttonFocusRingOutset + root.laneRingGap)
    // Between a box in the lane and the focus ring under it: at 1 px the
    // box read as a tab on the ring, joined to it.
    readonly property int laneRingGap: 3
    // The line's own leading, which grows with the font, is most of the
    // room around its figures; the style's 6 px above and below it made
    // the lane a third taller.
    readonly property real laneToolTipPadding: 2
    readonly property real laneBoxHeight: seekTimeBox.implicitHeight
    // Between a box in the lane and the window's top edge.
    readonly property int laneWindowTopMargin: 4
    // The room above the band that the lane borrows: the menu bar's row,
    // while the window shows one.
    readonly property var windowMenuBar: ApplicationWindow.menuBar
    readonly property real popupHeadroomAbove: root.windowMenuBar && root.windowMenuBar.visible
        ? root.windowMenuBar.height : 0
    // Whether the menu bar is in use: a menu of it open, a title under the
    // pointer or pressed, or the keyboard in it. The lane then gives the
    // row back, as a box there would stand over the title the user is
    // reaching for (with the pointer resting on the seek bar, Alt+P left
    // the time box over "Playback").
    readonly property bool windowMenuActive: {
        const bar = root.windowMenuBar
        if (!bar || !bar.visible) {
            return false
        }
        if (bar.activeFocus) {
            return true
        }
        for (let i = 0; i < bar.count; ++i) {
            const title = bar.itemAt(i)
            if (title && (title.hovered || title.down || title.activeFocus
                    || (title.menu && title.menu.visible))) {
                return true
            }
        }
        return false
    }
    // The lane holds one box at a time. The seek bar's time goes first,
    // then the tooltip of the control the pointer is on or holds; a
    // tooltip the keyboard's focus shows gives way to either (with the
    // pointer resting on the bar and Next focused, Next's box stood over
    // the time's, and its backdrop over the time).
    readonly property Item lanePointerControl: [previousButton, playPauseButton, stopButton, nextButton,
        muteButton, volumeSlider].find(control => control.hovered || control.pressed) || null
    // Whether the lane is free for the tooltip of `control`.
    function laneFreeFor(control) {
        return !root.windowMenuActive && !seekTimeBox.shown
            && (root.lanePointerControl === null || root.lanePointerControl === control)
    }
    // What the band grows by, above the row, in whole pixels: nothing
    // under the menu bar, whose row holds the lane; without one, as much
    // as the lane needs beyond the room above the row.
    readonly property real laneGrowth: Math.max(0, Math.ceil(root.laneBoxHeight + root.laneClearance
        + root.laneWindowTopMargin - root.rowHeight / 2 - root.popupHeadroomAbove))
    // The lane's bottom, in the transport's coordinates.
    readonly property real laneBottom: root.snapped(root.laneGrowth + root.rowHeight / 2 - root.laneClearance)
    // The y of a box in the lane, `boxHeight` tall, whose parent is
    // `rowItem`, an item of the row or of a group in it.
    function laneY(rowItem, boxHeight) {
        let rowY = 0
        for (let item = rowItem; item && item !== root.contentItem; item = item.parent) {
            rowY += item.y
        }
        return root.laneBottom - boxHeight - root.topPadding - rowY
    }
    // The band at rest is its row: under the menu's titles, the room the
    // menu bar leaves under them and the row balance by themselves.
    implicitHeight: root.rowHeight + root.laneGrowth
    // The content's inset, on both sides, measured to what is drawn, as
    // the transport's other gaps are: Previous's glyph starts where the
    // library's rows and the status bar's text do, with its button's clear
    // space in the inset (with the button's frame there, its ink was at
    // 20 px), and the volume groove ends at it. The button's focus ring, 3 px
    // outside it, stays in the window: at a large font, whose wider clear
    // space leaves less than that, the glyph starts a pixel or two further
    // in. On whole pixels, as the clear space can be a half.
    readonly property int edgeInset: 12
    readonly property int buttonFocusRingOutset: 3
    leftPadding: Math.max(root.buttonFocusRingOutset, Math.round(root.edgeInset - root.smallGlyphInset))
    rightPadding: root.edgeInset
    topPadding: root.laneGrowth
    bottomPadding: 0
    // Chrome. The seam under it is the main window's.
    background: Rectangle {
        objectName: "transportBackground"
        color: root.chromeColor
    }

    // A slider whose held arrow keys are one gesture, as a drag is. Qt's
    // autorepeat clears `pressed` at each repeat, but not `adjusting`.
    component GestureSlider: Slider {
        id: gestureSlider
        // The arrow keys held down.
        property var heldKeys: []
        readonly property bool adjusting: gestureHeld()
        readonly property bool pointerHeld: pointerHold.active
        // Where the holding pointer is, in the slider's coordinates, and
        // whether it is a touch. The hold clears its point as the release
        // arrives, before the slider takes the release there, so the
        // position is kept from the hold, the release's included.
        property point heldPointerPosition
        property bool heldByTouch: false
        // The holding pointer lifted, at heldPointerPosition; the slider
        // takes the release after this. Qt deactivates the hold before it
        // says a grab was cancelled, so a cancelled hold lifts it too.
        signal pointerLifted()

        // The mouse's or a touch's hold. Its passive grab leaves the drag
        // to the slider.
        PointHandler {
            id: pointerHold
            onPointChanged: {
                if (active) {
                    gestureSlider.heldPointerPosition = point.position
                }
            }
            // The point is still the event's here: the press's, or the
            // release's.
            onActiveChanged: {
                gestureSlider.heldPointerPosition = point.position
                if (active) {
                    gestureSlider.heldByTouch = point.device !== null
                        && point.device.type === PointerDevice.TouchScreen
                } else {
                    gestureSlider.pointerLifted()
                }
            }
        }

        // Whether a drag or held keys move the slider, from their own state.
        // A binding that also reads what `adjusting` decides can update
        // between the two, and see `adjusting` still unchanged.
        function gestureHeld() {
            return pressed || heldKeys.length > 0
        }

        // An arrow key's press, each autorepeat's too, before the slider
        // steps by it.
        signal arrowPressed(int key)

        // The slider handles the keys after these, unless they accept them.
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                if (!heldKeys.includes(event.key)) {
                    heldKeys = heldKeys.concat([event.key])
                }
                arrowPressed(event.key)
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
    // tokens rather than the style's: the hover tint, which fades, and the
    // stronger pressed tint while pressed. Keyboard focus is one 2 px ring 3 px outside
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
                ? root.pressedTint
                : root.hoverTint
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
            anchors.margins: -root.buttonFocusRingOutset
            radius: 6
            color: "transparent"
            border.width: 2
            border.color: root.uiPalette.focusRing
        }
    }

    // The box of every tooltip in the lane, the seek bar's time box's
    // first: the style's tooltip fill, square on the band with a 1 px edge
    // and no shadow, one style in both themes. The style's soft shadow,
    // 16 px under a box 22 px tall, spread into the gap between the box
    // and the waveform (light), and its edge, measured against the box's
    // fill, was lost on the lighter Header chrome around it, so the light
    // box read as a white card with a shadow and the dark one as a framed
    // readout. The edge is the style's, at least 1.8:1 on both the fill and
    // the chrome: as the style draws it in Breeze Dark (1.9:1), whose
    // tooltip fill is the chrome; in Breeze Light, whose fill is lighter
    // than the chrome, the style's edge was 1.5:1 on the chrome and soft
    // without the shadow. #a6a7a8 / #535659.
    readonly property real laneBoxEdgeContrast: 1.8
    readonly property color laneBoxFill: laneBoxColors.Kirigami.Theme.backgroundColor
    readonly property color laneBoxEdge: root.uiPalette.readableOn(
        ColorUtils.mixColor(root.laneBoxFill, laneBoxColors.Kirigami.Theme.textColor,
            laneBoxColors.Kirigami.Theme.frameContrast),
        [root.laneBoxFill, root.chromeColor], root.laneBoxEdgeContrast)
    Item {
        id: laneBoxColors
        Kirigami.Theme.colorSet: Kirigami.Theme.Tooltip
        Kirigami.Theme.inherit: false
    }
    component LaneBox: Rectangle {
        id: laneBox
        objectName: "transportLaneBox"
        radius: Kirigami.Units.cornerRadius
        color: root.laneBoxFill
        border.width: root.devicePixelLine
        border.color: root.laneBoxEdge

        // Over the menu bar's row, the titles the box covers or comes near
        // go under a backdrop in the menu bar's colour, whole: part of a
        // title beside the box read as a fragment of a word joined to it
        // ("Edit[Next · Ctrl+Right]k", "Pla[0:04]k"). It is the box's, so
        // it fades and goes with it and follows it along the bar; over
        // empty chrome there is none. It takes no input.
        Rectangle {
            objectName: "transportLaneBackdrop"
            z: -1
            readonly property rect span: root.laneBackdropSpan(laneBox)
            visible: span.width > 0
            x: span.x
            y: span.y
            width: span.width
            height: span.height
            color: root.menuBarFill
        }
    }
    // How near the box a title goes under the backdrop.
    readonly property int laneBackdropReach: 4
    // What the menu bar paints under its titles: its background's colour,
    // or the chrome.
    readonly property color menuBarFill: root.windowMenuBar && root.windowMenuBar.background
        && root.windowMenuBar.background.color !== undefined
        ? root.windowMenuBar.background.color : root.chromeColor
    // The backdrop of `box`, a lane box, in its coordinates: the menu bar's
    // height, across the titles within laneBackdropReach of it, or nothing.
    // Mapped from the places of the box in its popup, of the popup and of
    // the titles, which a binding of it then follows.
    function laneBackdropSpan(box) {
        const bar = root.windowMenuBar
        const popupItem = box.parent
        if (!bar || !bar.visible || !box.visible || !popupItem || !popupItem.parent) {
            return Qt.rect(0, 0, 0, 0)
        }
        const origin = popupItem.parent.mapToItem(bar, popupItem.x + box.x, popupItem.y + box.y)
        const reach = root.laneBackdropReach
        let left = Infinity
        let right = -Infinity
        for (let i = 0; i < bar.count; ++i) {
            const title = bar.itemAt(i)
            if (!title || !title.parent || !title.visible || title.width <= 0) {
                continue
            }
            const at = title.parent.mapToItem(bar, title.x, title.y)
            if (at.x < origin.x + box.width + reach && at.x + title.width > origin.x - reach
                    && at.y < origin.y + box.height + reach && at.y + title.height > origin.y - reach) {
                left = Math.min(left, at.x)
                right = Math.max(right, at.x + title.width)
            }
        }
        return right > left
            ? Qt.rect(left - origin.x, -origin.y, right - left, bar.height)
            : Qt.rect(0, 0, 0, 0)
    }

    // A tooltip in the lane over the row, the menu bar's row, borrowed
    // while it is open, of `rowItem`, an item of the row: one line, as the
    // time box is (the style's label wrapped Mute's to two lines at a large
    // font, past the lane's top). The lane keeps it off the window's top
    // edge, so the popup's own margin, which would push it down onto the
    // focus rings, is none.
    component LaneToolTip: Components.PlainToolTip {
        id: laneToolTip
        required property Item rowItem
        parent: rowItem
        y: root.laneY(rowItem, height)
        topMargin: 0
        topPadding: root.laneToolTipPadding
        bottomPadding: root.laneToolTipPadding
        background: LaneBox {}
        contentItem: Label {
            textFormat: Text.PlainText
            text: laneToolTip.text
            font: laneToolTip.font
        }
    }

    // A flat 32 px tool button of the transport with a theme icon. Its
    // tooltip names the action and its shortcut.
    component TransportButton: ToolButton {
        id: transportButton

        required property string name
        // The glyph's size: 16 px, 20 px for Play/Pause, larger with a
        // larger font.
        property int glyphSize: root.smallGlyphSize
        // Ferrous's own glyph, drawn for the size on whole pixels, which
        // scales as one with the others: a theme's larger icons are other
        // designs, with more space around the glyph.
        property url glyphSource
        // The action's text without its mnemonic, and its shortcut.
        readonly property string label: FormatUtils.withoutMnemonic(
            transportButton.action ? transportButton.action.text || "" : "")
        property string shortcutText: transportButton.action && transportButton.action.shortcut
            ? String(transportButton.action.shortcut)
            : ""
        // The text colour, also while the button is off: Kirigami draws a
        // disabled icon's tint as a grey at half its alpha, Breeze's
        // disabled look, once. Tinted the disabled text colour as well, the
        // glyph was dimmed twice and all but vanished on the chrome (about
        // 1.7:1 on Breeze Light's, 2.6:1 on Dark's; now about 2.9:1 and
        // 4.6:1, against 11.5:1 and 13.7:1 while on).
        readonly property color glyphColor: root.uiPalette.text
        // Whether it can act now. Off, the button is, and its action stays
        // on. Not a binding on `enabled`, which the button sets from its
        // action as it takes the action and at each change of the action's,
        // after the bindings that read them have run (ConditionalMenuItem):
        // the button follows both after it has.
        property bool canAct: true
        property var followedAction: null
        function followStates() {
            transportButton.enabled = transportButton.canAct
                && !!transportButton.action && transportButton.action.enabled
        }
        function followAction() {
            if (transportButton.followedAction) {
                transportButton.followedAction.enabledChanged.disconnect(transportButton.followStates)
            }
            transportButton.followedAction = transportButton.action
            if (transportButton.followedAction) {
                transportButton.followedAction.enabledChanged.connect(transportButton.followStates)
            }
            transportButton.followStates()
        }
        onCanActChanged: transportButton.followStates()
        onActionChanged: transportButton.followAction()
        Component.onCompleted: transportButton.followAction()

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
        readonly property string toolTipText: root.actionToolTip(transportButton.label, transportButton.shortcutText)
        // In the lane, and not while the button is held down.
        LaneToolTip {
            objectName: "transportButtonToolTip"
            rowItem: transportButton
            visible: (transportButton.hovered || transportButton.visualFocus) && !transportButton.down
                && root.laneFreeFor(transportButton)
            delay: Kirigami.Units.toolTipDelay
            message: transportButton.toolTipText
        }
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
                // At its size: Kirigami would draw a 38 px glyph of a
                // 24 pt font at 32 px.
                roundToIconSize: false
                source: transportButton.glyphSource
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
                id: previousButton
                name: "previous"
                canAct: root.previousCanAct
                glyphSource: Qt.resolvedUrl("../assets/transport-previous.svg")
                action: root.previousAction
            }

            // One button that shows the state: Pause while playing,
            // otherwise Play. Its two glyphs cross-fade over shortDuration,
            // the one shown going out as the other comes in, so the button
            // never goes blank (a sequential swap left it below half its
            // ink for 50 ms). Both opacities come from one
            // progress, so a change back mid-fade reverses from where it
            // is, and at rest one glyph is whole and the other gone.
            TransportButton {
                id: playPauseButton
                name: "playPause"
                canAct: root.playPauseCanAct
                // A step above its neighbours, within the same button.
                glyphSize: root.playPauseGlyphSize
                action: root.playing ? root.pauseAction : root.playAction
                // Space plays and pauses, as the tooltip says.
                shortcutText: root.playPauseAction && root.playPauseAction.shortcut
                    ? String(root.playPauseAction.shortcut)
                    : ""

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

                    // Ferrous's own Play and Pause, drawn for 20 px on whole
                    // pixels: the theme's are drawn for 16 and 22 px.
                    Kirigami.Icon {
                        objectName: "transportPlayPauseGlyph_play"
                        anchors.centerIn: parent
                        width: playPauseButton.glyphSize
                        height: playPauseButton.glyphSize
                        roundToIconSize: false
                        source: Qt.resolvedUrl("../assets/transport-play.svg")
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
                        roundToIconSize: false
                        source: Qt.resolvedUrl("../assets/transport-pause.svg")
                        color: playPauseButton.glyphColor
                        isMask: true
                        opacity: playPauseGlyphs.pauseProgress
                        visible: opacity > 0
                    }
                }
            }

            TransportButton {
                id: stopButton
                name: "stop"
                canAct: root.stopCanAct
                glyphSource: Qt.resolvedUrl("../assets/transport-stop.svg")
                action: root.stopAction
            }

            TransportButton {
                id: nextButton
                name: "next"
                canAct: root.nextCanAct
                glyphSource: Qt.resolvedUrl("../assets/transport-next.svg")
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
            // A track plays or is paused: the bar shows its playhead, at
            // the start until its length is known.
            readonly property bool trackActive: !root.stopped
            readonly property bool seekAllowed: durationKnown && trackActive
            // The bar: 28 px, with the ±12 px waveform in its middle and
            // the playhead across its height, 2 px past the waveform's
            // peaks on either side. A larger font scales the waveform as it
            // does the glyphs (unscaled, at 24 pt it stayed 24 px beside
            // 48 px glyphs).
            readonly property real waveformHeight: root.scaled(24)
            readonly property real boxHeight: waveformHeight + 4
            readonly property real playheadHeight: boxHeight
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
                : Math.round(shownVisualPosition * travelDevicePixels) / windowDevicePixelRatio
            // Where on the bar the shown time lies, from the left, as the
            // slider's visualPosition places its value.
            readonly property real shownVisualPosition: {
                const fraction = Math.max(0, Math.min(1, (shownSeconds - from) / (to - from)))
                return mirrored ? 1 - fraction : fraction
            }
            // The band of the visualization's view, from its left edge to
            // its right on whole device pixels, within the bar: while a
            // track of known length is loaded and the view's range is given.
            readonly property bool viewBandShown: seekAllowed
                && isFinite(root.viewStartSeconds) && isFinite(root.viewEndSeconds)
                && root.viewEndSeconds > root.viewStartSeconds
                && root.viewEndSeconds > from && root.viewStartSeconds < to
            // Where `seconds` is on the bar, in device pixels from its left.
            function barPixelOf(seconds) {
                const fraction = Math.max(0, Math.min(1, (seconds - from) / (to - from)))
                return (mirrored ? 1 - fraction : fraction) * barDevicePixels
            }
            readonly property real viewRangeLeftPixel: !viewBandShown ? 0
                : Math.min(barPixelOf(root.viewStartSeconds), barPixelOf(root.viewEndSeconds))
            readonly property real viewRangeRightPixel: !viewBandShown ? 0
                : Math.max(barPixelOf(root.viewStartSeconds), barPixelOf(root.viewEndSeconds))
            // However deep the zoom, the band stays wide enough to find,
            // and to show either side of the playhead on it: narrower, it
            // is drawn this wide about the middle of its range, on the bar.
            readonly property int viewBandMinimumWidth: 6
            readonly property int viewBandMinimumDevicePixels:
                Math.round(viewBandMinimumWidth * windowDevicePixelRatio)
            readonly property bool viewBandWidened: viewBandShown
                && Math.round(viewRangeRightPixel) - Math.round(viewRangeLeftPixel) < viewBandMinimumDevicePixels
            // The band moves with the view, in the frame the plot shows it
            // in, however it moved: a glide as a moved view followed again
            // left the band behind a plot that had already landed (H16).
            readonly property real viewBandLeft: (!viewBandWidened ? Math.round(viewRangeLeftPixel)
                : Math.max(0, Math.min(barDevicePixels - viewBandMinimumDevicePixels,
                    Math.round((viewRangeLeftPixel + viewRangeRightPixel - viewBandMinimumDevicePixels) / 2))))
                / windowDevicePixelRatio
            readonly property real viewBandRight: !viewBandWidened
                ? Math.round(viewRangeRightPixel) / windowDevicePixelRatio
                : viewBandLeft + viewBandMinimumDevicePixels / windowDevicePixelRatio
            // The handle is a grab target wider than the line and not drawn.
            // Padding of half their difference outside the bar puts the
            // handle's centre, which a press or drag moves under the
            // pointer, on the line's centre at every position.
            readonly property real grabTargetWidth: 10
            // How far its focus ring reaches outside the bar: 2 px clear of
            // the playhead, whose line spans the bar's height. At the
            // buttons' 3 px, the line's ends were 1 px from the ring, and at
            // the bar's start the line doubled the ring's side.
            readonly property int focusRingOutset: 4
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

            // The time the bar shows and a gesture seeks to, exactly: the
            // gesture's, under the pointer or keys, or otherwise playback's,
            // which moves by the bar's device pixels and stays where it was
            // while a gesture is held.
            property real playbackSeconds: 0
            readonly property real shownSeconds: isNaN(gestureValue) ? playbackSeconds : gestureValue
            // The slider's value is the second its description gives, as
            // assistive technology reads the value as a number with all its
            // decimals ("30.353452042"); within the slider's range, whose
            // end may fall within a second.
            function valueOfSecond(second) {
                return Math.max(from, Math.min(Math.floor(to), second))
            }

            // The second the texts show: a gesture's under the pointer or
            // keys, otherwise playback's, which changes every second while
            // the bar moves by pixels, which can be seconds apart.
            readonly property int valueSecond: adjusting ? Math.floor(shownSeconds) : root.positionSecond

            Accessible.name: I18n.i18nc("@label", "Seek")
            // The value's second, which a track's last half second would
            // otherwise give past its length.
            Accessible.description: !seekAllowed
                ? I18n.i18nc("@info", "No track playing")
                : root.endsEarlySeconds >= 0
                    ? I18n.i18nc("@info position of duration, then where a damaged file ends",
                        "%1 of %2. The file ends at %3.",
                        FormatUtils.formatClockTime(Math.floor(value)),
                        FormatUtils.formatClockTime(root.uiBridge.durationSeconds),
                        FormatUtils.formatClockTime(root.endsEarlySeconds))
                    : I18n.i18nc("@info position of duration", "%1 of %2",
                        FormatUtils.formatClockTime(Math.floor(value)),
                        FormatUtils.formatClockTime(root.uiBridge.durationSeconds))

            // The time the current gesture moved to, exactly, or NaN. The
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
                // A touch moves the slider once it drags.
                pointerMoving = pointerMoving || (!stepped && pressed)
                let target = value
                if (stepped && durationKnown) {
                    // A step of the keys or the wheel moves from the time a
                    // held step reached, or at first from the position now,
                    // which the bar shows only to its last pixel.
                    const start = isNaN(gestureValue) ? root.currentPositionSeconds() : gestureValue
                    const step = value - valueBeforeChange
                    if (heldKeys.length > 0) {
                        // A key's whole step, which the slider cuts short at
                        // an end that its whole second can reach first.
                        target = Math.max(from, Math.min(to, start + Math.sign(step) * stepSize))
                    } else if (value > from && value < to) {
                        // A step of the wheel to either end stays there.
                        target = Math.max(from, Math.min(to, start + step))
                    }
                }
                if (adjusting) {
                    gestureValue = target
                } else {
                    playbackSeconds = target
                    if (seekAllowed) {
                        root.seekCommitted(target)
                    }
                }
                // A gesture's second, as valueSecond's, or the one playback
                // shows once it is there.
                value = valueOfSecond(adjusting ? Math.floor(target) : Math.round(target))
            }
            // The value, a whole second, can be at an end of the range while
            // the time is not, and then a step towards that end leaves it
            // where it is, so the slider does not move: the time steps to the
            // end itself.
            onArrowPressed: function(key) {
                const forward = (key === Qt.Key_Right) !== mirrored
                const end = forward ? to : from
                if (durationKnown && value === end && shownSeconds !== end) {
                    gestureValue = end
                }
            }
            // A pointer's time is the one under it, which the slider gives
            // only by moving its value, a whole second, to another: one on
            // that second, as at an end the value is already at, moves
            // nothing. So the time comes from the pointer's position,
            // wherever the slider takes it: a mouse's press and drag, a
            // touch's drag once it moves the slider, and any release.
            property bool pointerMoving: false
            function takePointer() {
                gestureValue = secondsAtX(heldPointerPosition.x)
                value = valueOfSecond(Math.floor(gestureValue))
            }
            onPressedChanged: {
                pointerMoving = pressed && pointerHeld && !heldByTouch
                if (pointerMoving) {
                    takePointer()
                }
            }
            onHeldPointerPositionChanged: {
                if (pointerMoving && pressed) {
                    takePointer()
                }
            }
            onPointerLifted: {
                // Before the slider lets go, so the hover has the pointer
                // as the gesture ends.
                releasedOnBar = !heldByTouch && contains(heldPointerPosition)
                handedToHover = true
                if (pressed) {
                    takePointer()
                }
            }
            onPointerHeldChanged: {
                if (pointerHeld) {
                    handedToHover = false
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
                    color: root.chromeColor
                }

                // The band of the visualization's view, across the bar's
                // height behind the waveform: decoration, which assistive
                // technology does not see.
                Rectangle {
                    objectName: "seekViewBand"
                    visible: seekSlider.viewBandShown
                    antialiasing: false
                    x: seekSlider.viewBandLeft
                    width: seekSlider.viewBandRight - seekSlider.viewBandLeft
                    height: parent.height
                    color: root.seekViewBandColor
                }

                // Without peaks: a 1 px line through the middle. With them,
                // the waveform draws the line where it has nothing to draw
                // yet.
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
                    // ±12 px about the middle at the default font, on whole
                    // device pixels.
                    y: root.snapped((parent.height - seekSlider.waveformHeight) / 2)
                    height: seekSlider.waveformHeight
                    // Only with peaks: it lets what is under it show, the
                    // empty line too.
                    visible: root.hasWaveform
                    peaksData: root.stopped && !root.hasWaveform
                        ? ""
                        : root.uiBridge.waveformPeaksPacked
                    generatedSeconds: root.uiBridge.waveformCoverageSeconds
                    waveformComplete: root.uiBridge.waveformComplete
                    positionSeconds: root.displayedPositionSeconds
                    durationSeconds: root.uiBridge.durationSeconds
                    barColor: root.seekPlayedColor
                    // The chrome and the band show through it.
                    bgColor: "transparent"
                    // Past the waveform's end, as without one.
                    emptyLineColor: root.seekEmptyLineColor
                }

                // The played part where the band is: the band's colour over
                // the waveform before the playhead's line, at 15 % (10 % on
                // the light theme).
                Rectangle {
                    objectName: "seekViewBandPlayed"
                    readonly property real coverLeft: Math.min(seekSlider.viewBandLeft, seekSlider.playheadX)
                    readonly property real coverRight: Math.min(seekSlider.viewBandRight, seekSlider.playheadX)
                    visible: seekSlider.viewBandShown && width > 0
                    antialiasing: false
                    x: coverLeft
                    width: Math.max(0, coverRight - coverLeft)
                    y: waveformItem.y
                    height: waveformItem.height
                    color: root.seekViewBandPlayedCover
                }

                // The unplayed part: the chrome over the waveform after the
                // playhead's line, which leaves it at the unplayed opacity,
                // and the band's colour where the band is, so the band is
                // the same on either side of the playhead. Stopped, all of
                // the waveform is unplayed.
                Item {
                    id: seekUnplayed
                    objectName: "seekUnplayed"
                    visible: seekSlider.trackActive || root.hasWaveform
                    x: seekSlider.playheadX
                    width: Math.max(0, parent.width - x)
                    y: waveformItem.y
                    height: waveformItem.height
                    // Where the band is in it.
                    readonly property real bandLeft: seekSlider.viewBandShown
                        ? Math.max(0, Math.min(width, seekSlider.viewBandLeft - x)) : width
                    readonly property real bandRight: seekSlider.viewBandShown
                        ? Math.max(bandLeft, Math.min(width, seekSlider.viewBandRight - x)) : width

                    Rectangle {
                        antialiasing: false
                        width: seekUnplayed.bandLeft
                        height: parent.height
                        color: root.seekUnplayedCover
                    }
                    Rectangle {
                        objectName: "seekViewBandUnplayed"
                        visible: width > 0
                        antialiasing: false
                        x: seekUnplayed.bandLeft
                        width: seekUnplayed.bandRight - seekUnplayed.bandLeft
                        height: parent.height
                        color: root.seekViewBandUnplayedCover
                    }
                    Rectangle {
                        antialiasing: false
                        x: seekUnplayed.bandRight
                        width: seekUnplayed.width - seekUnplayed.bandRight
                        height: parent.height
                        color: root.seekUnplayedCover
                    }
                }

                // A file that ends early: the glyph of its issue's severity,
                // a warning, in the colour the status bar and Recent Issues
                // give it (it was red, an error's colour, beside the status
                // bar's grey warning for the same issue), at
                // the cut, which the waveform alone didn't explain, in a gap
                // of the chrome as tall as the waveform, so that no slivers
                // of it stand over and under the sign. Under the playhead,
                // on whole pixels and within the bar.
                Rectangle {
                    objectName: "seekEndsEarlyMark"
                    visible: root.endsEarlySeconds >= 0
                    antialiasing: false
                    width: root.smallGlyphSize
                    height: waveformItem.height
                    x: !visible ? 0 : Math.max(0, Math.min(Math.floor(parent.width - width),
                        Math.round(seekSlider.barPixelOf(root.endsEarlySeconds)
                            / seekSlider.windowDevicePixelRatio - width / 2)))
                    y: waveformItem.y
                    color: root.chromeColor

                    Kirigami.Icon {
                        objectName: "seekEndsEarlyGlyph"
                        y: Math.round((parent.height - height) / 2)
                        width: parent.width
                        height: width
                        roundToIconSize: false
                        source: Qt.resolvedUrl("../assets/" + ErrorUtils.glyphAsset(ErrorUtils.EndsEarlyGlyph))
                        isMask: true
                        color: ErrorUtils.glyphColor(ErrorUtils.EndsEarlyGlyph, root.uiPalette)
                    }
                }

                // A 1 px line at the hovering pointer, which comes and goes
                // with the time box over it: at the box's opacity while the
                // box shows, or fades out, the hover's time, where that last
                // was. A gesture shows its time at the playhead with no line.
                // It is under the playhead and shows only with 4 px clear
                // between it and the playhead's line: on it or nearer, the
                // playhead alone marks the box's time (2 or 3 px beside it,
                // after a click, the two drew as one doubled line under the
                // box). After a click or a
                // drag released on the bar it is the box's line as at any
                // hover, at the pointer, so the box never stands beside a
                // playhead that playback moves on with no line under it
                // (the line waited for the pointer to move, and the box
                // stayed without it).
                Rectangle {
                    id: seekHoverLine
                    objectName: "seekHoverLine"
                    opacity: seekTimeBox.showsHover && seekTimeBox.contentItem.visible
                        && !seekHoverLine.atPlayhead() ? seekTimeBox.opacity : 0
                    visible: opacity > 0
                    antialiasing: false
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: Math.max(0, Math.min(parent.width - width,
                        Math.floor(seekSlider.hoverX * seekSlider.windowDevicePixelRatio)
                            / seekSlider.windowDevicePixelRatio))
                    width: root.devicePixelLine
                    color: root.seekHoverLineColor
                    function atPlayhead() {
                        const clear = 4
                        return seekPlayhead.visible
                            && x + width > seekSlider.playheadX - clear
                            && x < seekSlider.playheadX + seekSlider.playheadLineWidth + clear
                    }
                }

                // The playhead: its casing, cut to the bar at either end,
                // with the line in it.
                Rectangle {
                    id: seekPlayhead
                    objectName: "seekPlayhead"
                    visible: seekSlider.trackActive
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

                // Keyboard focus: one 2 px ring 2 px clear of the bar.
                Rectangle {
                    objectName: "seekFocusRing"
                    visible: seekSlider.visualFocus
                    anchors.fill: parent
                    anchors.margins: -seekSlider.focusRingOutset
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
            // under the pointer, and never moves the slider or seeks. Off
            // while the window's pointer is a finger: at each frame, Qt
            // hovers what is under a held touch with an event that looks
            // like the mouse's, which the accepted devices let through.
            HoverHandler {
                id: seekHover
                objectName: "seekHover"
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                enabled: !seekPointer.touch
                // The hover has caught up with the pointer.
                onHoveredChanged: seekSlider.handedToHover = false
                onPointChanged: seekSlider.handedToHover = false
            }
            WindowPointer {
                id: seekPointer
                window: seekSlider.Window.window
            }

            // A drag hands the pointer over to the hover where it lifts. Qt
            // sends no hover during a drag, so the handler still has the
            // press's point, and that it is over the bar, until it catches
            // up, at the next frame or move: until then the hover is the
            // release's.
            property bool handedToHover: false
            property bool releasedOnBar: false
            readonly property bool pointerOnBar: handedToHover ? releasedOnBar : seekHover.hovered
            readonly property real pointerX: handedToHover
                ? heldPointerPosition.x : seekHover.point.position.x
            // The pointer has a time to show, which a gesture's goes before.
            // It doesn't follow `adjusting`: as that ended at a release, what
            // read both saw neither for a moment, and the box went out and
            // faded in again.
            readonly property bool hoverTimeShown: pointerOnBar && seekAllowed
            // Where the hovering pointer is, in the slider's coordinates, and
            // the second a click there would seek to, as a drag there shows
            // it. Hidden, they keep their last values, so the tooltip fades
            // out where it was, with its time, and nothing hidden moves.
            property real hoverX: 0
            property int hoverSecond: 0
            Binding on hoverX {
                when: seekSlider.hoverTimeShown
                value: seekSlider.pointerX
                restoreMode: Binding.RestoreNone
            }
            Binding on hoverSecond {
                when: seekSlider.hoverTimeShown
                // Without a length, whose stand-in range puts the start at
                // every point, the last second stays. From the bridge, as
                // `durationKnown` and `hoverTimeShown` can still hold the
                // last length's values when this updates.
                value: isFinite(root.uiBridge.durationSeconds) && root.uiBridge.durationSeconds > 0
                    ? Math.floor(seekSlider.secondsAtX(seekSlider.pointerX))
                    : seekSlider.hoverSecond
                restoreMode: Binding.RestoreNone
            }

            // The time in a native tooltip in the transport's lane, the
            // menu bar's row, borrowed while it is open, over the line it
            // names: the playhead while a gesture moves it, otherwise the
            // hovering pointer's line. It hides none of the waveform it
            // reads (beside the line it covered some 40 px of it where the
            // pointer was).
            ToolTip {
                id: seekTimeBox
                objectName: "seekTimeBox"
                parent: seekSlider
                // It follows the pointer and the gesture, not clicks or keys.
                closePolicy: Popup.NoAutoClose
                readonly property bool shown: (seekSlider.adjusting || seekSlider.hoverTimeShown)
                    && seekSlider.seekAllowed && !root.windowMenuActive
                // Whether the time it shows, or last showed, is the hover's
                // rather than a gesture's: the hover line then shows, and
                // fades out, with it.
                property bool showsHover: false
                // From the gesture's and the hover's own state, as the
                // anchor's: a drag released off the bar showed none.
                Binding on showsHover {
                    value: seekSlider.gestureHeld() ? false
                        : seekSlider.pointerOnBar ? true : seekTimeBox.showsHover
                    restoreMode: Binding.RestoreNone
                }
                // A hover's time comes after a moment, so a pointer that
                // crosses the bar on its way elsewhere shows none; a
                // gesture's at once, as does a hover's that comes back
                // while the last one still fades out.
                readonly property int hoverDelay: 80
                // Set together with the delay, which the tooltip reads when
                // it is made visible: a binding of each could see the other
                // still unchanged.
                visible: false
                delay: 0
                function showOrHide() {
                    delay = seekSlider.gestureHeld() || contentItem.visible ? 0 : hoverDelay
                    visible = shown
                }
                onShownChanged: showOrHide()
                Connections {
                    target: seekSlider
                    // A press on a hover still waiting shows the time at once.
                    function onAdjustingChanged() {
                        if (seekTimeBox.shown && !seekTimeBox.contentItem.visible) {
                            seekTimeBox.showOrHide()
                        }
                    }
                }
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
                        : seekSlider.pointerOnBar ? seekSlider.hoverX : seekTimeBox.anchorX
                    restoreMode: Binding.RestoreNone
                }
                // Centred on the line, within the bar at either end.
                readonly property real centredX: anchorX - width / 2
                readonly property real placedX: root.snapped(Math.max(0,
                    Math.min(seekSlider.width - width, centredX)))
                x: placedX
                // At the lane's bottom, clear of the bar's focus ring, at
                // every point of the bar, over the menu's titles there:
                // there is no other place to jump to (near the titles it
                // went under the bar, ~54 px down, over the now-playing
                // title at 24 pt). Its padding is the lane's.
                y: root.laneY(seekSlider, height)
                topPadding: root.laneToolTipPadding
                bottomPadding: root.laneToolTipPadding
                topMargin: 0
                background: LaneBox {}
                // It comes in over the short duration of the window's other
                // hovers, and goes as the style's tooltips do.
                enter: Transition {
                    NumberAnimation {
                        property: "opacity"
                        from: 0.0
                        to: 1.0
                        duration: Kirigami.Units.shortDuration
                        easing.type: Easing.OutCubic
                    }
                }
                // The time while shown, and nothing while hidden, so that
                // playback's seconds change no hidden text, which would draw
                // frames. The text keeps the last time while it fades out.
                // Its source is chosen as the anchor's is.
                readonly property string shownText: !shown ? ""
                    : seekSlider.gestureHeld() ? FormatUtils.formatClockTime(seekSlider.valueSecond)
                    : seekSlider.pointerOnBar ? FormatUtils.formatClockTime(seekSlider.hoverSecond)
                    : ""
                onShownTextChanged: {
                    if (shownText.length > 0) {
                        text = shownText
                    }
                }

                contentItem: Label {
                    objectName: "seekDragTime"
                    textFormat: Text.PlainText
                    text: seekTimeBox.text
                    font.family: seekTimeBox.font.family
                    font.pointSize: seekTimeBox.font.pointSize
                    font.features: root.uiPalette.tabularFigures
                }
            }
        }

        Binding {
            target: seekSlider
            property: "playbackSeconds"
            value: seekSlider.durationKnown ? root.displayedPositionSeconds : 0
            when: !seekSlider.adjusting
            // A gesture keeps the position it starts from.
            restoreMode: Binding.RestoreNone
        }
        Binding {
            target: seekSlider
            property: "value"
            // The second the description gives.
            value: seekSlider.durationKnown ? seekSlider.valueOfSecond(root.positionSecond) : 0
            when: !seekSlider.adjusting
            // A gesture keeps the position it starts from; restoring the
            // value from before the binding would make the arrow keys step
            // from it.
            restoreMode: Binding.RestoreNone
        }

        // "1:17 / 3:49": the position in the text colour, the length in the
        // secondary one. Stopped, all of it in the secondary colour and no
        // position, as there is no playhead: "–:–– / 3:49" with a track
        // loaded, its length the queue's until the engine has it, and
        // "–:–– / –:––" without, which fills the box as a time does, so the
        // volume keeps its distance from what is drawn ("–:––" alone left a
        // hole of some 45 px before it).
        Item {
            id: transportTime
            objectName: "transportTime"
            readonly property string noTime: I18n.i18nc("@info:status no time", "–:––")
            readonly property string lengthText: root.stopped && !root.stoppedTrack
                ? transportTime.noTime
                : seekSlider.durationKnown
                    ? FormatUtils.clockText(root.uiBridge.durationText)
                    : root.stoppedTrack && (root.uiBridge.playingQueueLengthText || "").length > 0
                        ? root.uiBridge.playingQueueLengthText
                        : transportTime.noTime
            readonly property string text: timePosition.text + timeLength.text

            Layout.leftMargin: root.seekToTimeGap
            Layout.alignment: Qt.AlignVCenter
            // As wide as a time in minutes, "0:00 / 0:00", which ends where
            // the box does, so that the volume glyph keeps its distance
            // from what is drawn; or what a longer time needs. The figures
            // are tabular, so every time in minutes is as wide, and the
            // position is never wider than the length. Stopped, the box
            // keeps its width, so that the volume doesn't move.
            Layout.preferredWidth: Math.max(Math.ceil(minutesMetrics.advanceWidth),
                Math.ceil(timeMetrics.advanceWidth))
            Layout.preferredHeight: timeRow.implicitHeight

            TextMetrics {
                id: timeMetrics
                font: timePosition.font
                text: transportTime.lengthText + timeLength.text
            }

            TextMetrics {
                id: minutesMetrics
                font: timePosition.font
                text: FormatUtils.clockText("00:00")
                    + I18n.i18nc("@info:status the track's length after its position", " / %1",
                        FormatUtils.clockText("00:00"))
            }

            Row {
                id: timeRow
                anchors.verticalCenter: parent.verticalCenter

                Label {
                    id: timePosition
                    objectName: "transportTimePosition"
                    font.features: root.uiPalette.tabularFigures
                    color: root.stopped ? root.uiPalette.textSecondary : root.uiPalette.text
                    textFormat: Text.PlainText
                    // While a drag or held keys move the playhead, the
                    // gesture's second, as the playhead and its time box
                    // show it (the readout kept playback's, and the bar
                    // contradicted it for the whole scrub); at the release
                    // the seek lands and playback's takes over at once.
                    text: root.stopped
                        ? transportTime.noTime
                        : !isNaN(seekSlider.gestureValue)
                            ? FormatUtils.formatClockTime(Math.floor(seekSlider.gestureValue))
                            : FormatUtils.clockText(root.positionText)
                }

                Label {
                    id: timeLength
                    objectName: "transportTimeLength"
                    font.features: root.uiPalette.tabularFigures
                    color: root.uiPalette.textSecondary
                    textFormat: Text.PlainText
                    text: I18n.i18nc("@info:status the track's length after its position", " / %1",
                        transportTime.lengthText)
                }
            }
        }

        ToolButton {
            id: muteButton
            objectName: "muteButton"
            // 24 px from the time to the glyph.
            Layout.leftMargin: Math.max(0, root.timeToVolumeGap - root.smallGlyphInset)
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: root.buttonSize
            implicitHeight: root.buttonSize
            padding: 0
            display: AbstractButton.IconOnly
            flat: true
            text: root.volumeMuted
                ? I18n.i18nc("@action:button", "Unmute")
                : I18n.i18nc("@action:button", "Mute")
            // Ferrous's own glyphs, at the weight of the transport's beside
            // them (Breeze's speaker, with 1 px waves, read lighter than
            // Next), and at their size.
            icon.source: root.volumeSilent
                ? Qt.resolvedUrl("../assets/volume-muted.svg")
                : Qt.resolvedUrl("../assets/volume.svg")
            icon.color: root.uiPalette.text
            icon.width: root.smallGlyphSize
            icon.height: root.smallGlyphSize
            // "Mute · Ctrl+Shift+M", as the other buttons name theirs.
            readonly property string toolTipText: root.actionToolTip(muteButton.text,
                root.muteAction && root.muteAction.shortcut ? String(root.muteAction.shortcut) : "")
            // In the lane, as the other buttons' are, not while held down.
            LaneToolTip {
                objectName: "transportButtonToolTip"
                rowItem: muteButton
                visible: (muteButton.hovered || muteButton.visualFocus) && !muteButton.down
                    && root.laneFreeFor(muteButton)
                delay: Kirigami.Units.toolTipDelay
                message: muteButton.toolTipText
            }
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
                    roundToIconSize: false
                    source: muteButton.icon.source
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
            Layout.leftMargin: root.muteButtonToSliderGap
            Layout.alignment: Qt.AlignVCenter
            // 96 px, 72 px in a narrow window; a larger font widens the
            // groove with the thumb and the glyphs, up to half again
            // (at 24 pt a 29 px thumb crowded a fixed 96 px groove).
            Layout.preferredWidth: volumeSlider.grooveWidth(root.narrow)
            Layout.preferredHeight: volumeSlider.boxHeight
            padding: 0
            // In whole percents, which assistive technology reads as the
            // value: "50" rather than "0.5".
            from: 0
            to: 100
            // The arrow keys' step; a drag moves by whole percents, as the
            // slider rounds a pointer's value to its step.
            stepSize: pressed && heldKeys.length === 0 ? 1 : 10
            hoverEnabled: true
            readonly property string percentText: I18n.i18nc("@info volume in percent", "%1%",
                Math.round(value))
            readonly property string toolTipText: root.volumeMuted
                ? I18n.i18nc("@info:tooltip volume in percent, while muted", "Volume %1, muted",
                    volumeSlider.percentText)
                : I18n.i18nc("@info:tooltip volume in percent", "Volume %1", volumeSlider.percentText)
            // A 3 px groove and a 12 px thumb in a 20 px box, both centred
            // on the groove's middle, which lies on whole device pixels; a
            // larger font scales them as it does the glyphs (unscaled, at
            // 24 pt the thumb stayed 12 px beside 38 px glyphs).
            readonly property real boxHeight: root.scaled(20)
            readonly property real grooveHeight: root.scaled(3)
            readonly property real grooveY: root.snapped((height - grooveHeight) / 2)
            readonly property real thumbSize: root.scaled(12)
            function grooveWidth(narrow) {
                return Math.round((narrow ? 72 : 96) * Math.min(1.5, root.fontScale))
            }
            Accessible.name: I18n.i18nc("@label", "Volume")
            Accessible.description: volumeSlider.percentText
            // Every move, including a release's, sends its value, so the
            // volume follows a drag or a held key.
            onMoved: root.setAppVolume(value / 100)

            // Its own tooltip, rather than the shared one, whose text a
            // drag or a key that moves the slider before the hover's delay
            // ends left at the volume it had ("Volume 100%" with the thumb
            // at 40%). A press shows it at once, a hover's still waiting
            // too.
            LaneToolTip {
                id: volumeToolTip
                objectName: "volumeToolTip"
                rowItem: volumeSlider
                readonly property bool wanted: (volumeSlider.hovered || volumeSlider.visualFocus
                    || volumeSlider.pressed) && root.laneFreeFor(volumeSlider)
                // Set together with the delay, which the tooltip reads as it
                // is made visible: a binding of each could see the other
                // still unchanged, and a press within the hover's delay then
                // waited the delay out again.
                visible: false
                delay: 0
                function showOrHide() {
                    delay = volumeSlider.pressed ? 0 : Kirigami.Units.toolTipDelay
                    visible = wanted
                }
                onWantedChanged: showOrHide()
                Connections {
                    target: volumeSlider
                    function onPressedChanged() { volumeToolTip.showOrHide() }
                }
                message: volumeSlider.toolTipText
            }

            background: Item {
                implicitWidth: volumeSlider.grooveWidth(false)
                implicitHeight: volumeSlider.boxHeight

                Rectangle {
                    objectName: "volumeGroove"
                    x: 0
                    width: parent.width
                    y: volumeSlider.grooveY
                    height: volumeSlider.grooveHeight
                    radius: height / 2
                    color: root.volumeUnfilledColor
                }

                Rectangle {
                    objectName: "volumeGrooveFill"
                    x: 0
                    width: volumeSlider.handle
                        ? volumeSlider.handle.x + volumeSlider.handle.width / 2
                        : 0
                    y: volumeSlider.grooveY
                    height: volumeSlider.grooveHeight
                    radius: height / 2
                    color: root.volumeMuted ? root.volumeUnfilledColor : root.volumeFilledColor

                    // As the thumb's, so a mute dims both together.
                    Behavior on color {
                        enabled: Kirigami.Units.shortDuration > 0
                        ColorAnimation {
                            duration: Kirigami.Units.shortDuration
                            easing.type: Easing.OutCubic
                        }
                    }
                }
            }

            // The solid thumb: the played colour, the unplayed one at rest
            // while muted, as the fill, and the active colour while hovered
            // or dragged. Keyboard focus draws a 2 px ring around it.
            handle: Rectangle {
                objectName: "volumeThumb"
                readonly property bool tinted: volumeSlider.hovered || volumeSlider.pressed
                x: volumeSlider.leftPadding + volumeSlider.visualPosition
                    * (volumeSlider.availableWidth - width)
                y: volumeSlider.grooveY + (volumeSlider.grooveHeight - height) / 2
                implicitWidth: volumeSlider.thumbSize
                implicitHeight: volumeSlider.thumbSize
                radius: width / 2
                color: tinted
                    ? root.volumeThumbActiveColor
                    : (root.volumeMuted ? root.volumeUnfilledColor : root.volumeFilledColor)
                border.width: 0

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

        // The volume, or while muted the one a mute put aside, so the thumb
        // stays where it was.
        Binding {
            target: volumeSlider
            property: "value"
            value: Math.round((root.volumeMuted ? root.mutedVolume : root.uiBridge.volume) * 100)
            when: !volumeSlider.adjusting
            // As for the seek slider.
            restoreMode: Binding.RestoreNone
        }
    }
}
