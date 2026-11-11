// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils

// The status bar's Last.fm slot (#373, #470): the Last.fm glyph, with a
// short bar beneath it, and a few words before it when something needs
// saying (#373 H6: a 10 px glyph inside a ring was a smudge at 1x, and
// its states looked alike).
// The bar is the glyph's: as wide as it, 2 px tall and 2 px under it, with
// no trough, no rounded ends and none of the scan bar's colours, so that
// it reads as part of the glyph and not as a second scan or a download.
// - Listening: the glyph in the secondary colour, and the bar in the text
//   colour growing under it with the listening done towards the scrobble;
//   the tooltip says how much is left. Both come from the scrobbler's
//   listened time, which a seek doesn't move and a pause holds; a new play
//   (a new track, a replay, Repeat One) starts it over. Before any of it,
//   the glyph alone.
// - Not scrobbled: once playback has stopped, by the end of the queue or
//   a stop, after a play that ended before its scrobble point. "Not
//   scrobbled" and the glyph in the tertiary colour, without the bar; the
//   tooltip says how far the play got. A seek never decides it: the
//   countdown goes on while the track plays.
// - Being sent: the bar full in the text colour, from the moment the play
//   reaches its scrobble point, and the glyph at half strength while the
//   send goes on in the background.
// - Scrobbled: once Last.fm has accepted it, the full bar settles in a
//   quiet grey under the glyph at full strength, so that the routine
//   success is no louder than the status bar's other words, and apart from
//   being sent by both the bar and the glyph.
// - Queued: "3 scrobbles waiting" and the offline glyph, only while
//   scrobbles stay queued: after a send failed, or while Last.fm holds
//   them back.
// - Failed: "Scrobble failed", and the glyph and the full bar in the
//   negative colour: the slot's only colour.
// - Paused by the user: the glyph struck through, in the tertiary colour,
//   without the bar.
// - Otherwise (not eligible, skipped, signed out): nothing.
// Clicking it pauses or resumes scrobbling; its menu has the settings.
AbstractButton {
    id: root

    required property var uiBridge
    required property var uiPalette

    // The Last.fm preferences were asked for.
    signal settingsRequested()

    readonly property bool shown: root.uiBridge.lastFmShowScrobbleProgress
        && root.uiBridge.lastFmScrobblingEnabled
        && root.uiBridge.lastFmAuthState === 2
    readonly property bool paused: root.uiBridge.lastFmScrobblingPaused
    readonly property int scrobbleState: root.uiBridge.lastFmScrobbleState
    readonly property int ineligibility: root.uiBridge.lastFmScrobbleIneligibility
    readonly property int listenedMs: root.uiBridge.lastFmScrobbleListenedMs
    readonly property int thresholdMs: root.uiBridge.lastFmScrobbleThresholdMs
    readonly property int pendingCount: Math.max(0, Number(root.uiBridge.lastFmPendingScrobbleCount) || 0)

    // What the slot shows: "hidden", "listening", "notScrobbled",
    // "scrobbled", "queued", "failed", "paused" or "empty". The scrobble
    // states and reasons are BinaryBridgeCodec::ScrobbleState and
    // ScrobbleIneligibility.
    readonly property string look: {
        if (!root.shown) {
            return "hidden"
        }
        if (root.paused) {
            return "paused"
        }
        if (root.scrobbleState === 4) {
            return "failed"
        }
        // Scrobbles wait while Last.fm can't be reached, or holds them
        // back. A scrobble on its way for the first time doesn't wait: the
        // bar fills at once, and the slot says it waits only if the send
        // fails.
        if (root.pendingCount > 0) {
            return "queued"
        }
        if (root.scrobbleState === 1) {
            return "listening"
        }
        if (root.scrobbleState === 2 || root.scrobbleState === 3) {
            return "scrobbled"
        }
        if (root.scrobbleState === 6 && root.ineligibility === 3) {
            return "notScrobbled"
        }
        return "empty"
    }
    readonly property bool occupied: root.look !== "hidden" && root.look !== "empty"

    // The listening done and still needed, in whole seconds: the slot
    // changes at most once a second while the track plays, and not at all
    // while it is paused. The scrobble point is whole seconds.
    readonly property int listenedSeconds: Math.floor(Math.max(0, root.listenedMs) / 1000)
    readonly property int thresholdSeconds: Math.ceil(Math.max(0, root.thresholdMs) / 1000)
    readonly property int secondsLeft: Math.max(0, root.thresholdSeconds - root.listenedSeconds)

    // The glyph: 16 px, drawn for 1x on whole pixels, and larger with a
    // larger font, as the status bar's other glyphs are.
    readonly property int glyphSize: root.uiPalette.scaledSize(Kirigami.Units.iconSizes.small)
    // The bar under it: the glyph's width, 2 px tall and 2 px below it at
    // the default font, a little more with a larger one, within the status
    // bar's height.
    readonly property int barWidth: root.glyphSize
    readonly property int barHeight: Math.max(2, Math.round(root.glyphSize / 12))
    readonly property int barGap: root.barHeight
    readonly property bool barShown: root.look === "listening" || root.look === "scrobbled"
        || root.look === "failed"
    // How much of the bar is drawn, 0 to 1: the listening done, by whole
    // device pixels of the bar, so a long scrobble point redraws it more
    // rarely still; full once the play has reached its scrobble point.
    readonly property real barProgress: {
        if (root.look === "scrobbled" || root.look === "failed") {
            return 1
        }
        if (root.look !== "listening" || root.thresholdSeconds <= 0) {
            return 0
        }
        const steps = Math.max(1, Math.round(root.barWidth * Math.max(1, Screen.devicePixelRatio)))
        return Math.floor(Math.min(1, root.listenedSeconds / root.thresholdSeconds) * steps) / steps
    }
    // Last.fm accepted the scrobble.
    readonly property bool scrobbleAccepted: root.look === "scrobbled" && root.scrobbleState === 3
    // The bar once scrobbled: opaque, the least of the text colour over the
    // status bar that is 3.5:1 on it, so that it still reads (WCAG 1.4.11)
    // and is quieter than the status bar's words. A grey of its own: the
    // secondary and tertiary text colours are bluish and, in a light
    // theme, all but the scan bar's fill.
    readonly property color settledBarColor: ColorUtils.mixColor(root.uiPalette.chrome,
        root.uiPalette.text,
        ColorUtils.opacityForContrast(root.uiPalette.text, root.uiPalette.chrome, 0.2, 3.5))
    // The bar: the text colour while listening and sending, the settled
    // grey once scrobbled, and the negative colour when Last.fm refused the
    // scrobble. Never the scan bar's colours.
    readonly property color barColor: {
        if (root.look === "failed") {
            return root.uiPalette.negative
        }
        return root.scrobbleAccepted ? root.settledBarColor : root.uiPalette.text
    }

    // The slot's words, before the glyph.
    readonly property string labelText: {
        switch (root.look) {
        case "queued":
            return I18n.i18ncp("@info:status", "%1 scrobble waiting", "%1 scrobbles waiting",
                root.pendingCount)
        case "failed":
            return I18n.i18nc("@info:status", "Scrobble failed")
        case "notScrobbled":
            return I18n.i18nc("@info:status", "Not scrobbled")
        default:
            return ""
        }
    }

    // What the slot says: its tooltip and accessible name.
    readonly property string stateText: {
        switch (root.look) {
        case "listening":
            return root.thresholdSeconds > 0
                ? I18n.i18nc("@info:tooltip %1 is a duration", "Last.fm: %1 to scrobble",
                    FormatUtils.formatClockTime(root.secondsLeft))
                : I18n.i18nc("@info:tooltip", "Last.fm: listening")
        case "notScrobbled":
            return I18n.i18nc("@info:tooltip %1 and %2 are durations",
                "Last.fm: not scrobbled. The play ended after %1 of the %2 of listening it needed.",
                FormatUtils.formatClockTime(root.listenedSeconds),
                FormatUtils.formatClockTime(root.thresholdSeconds))
        case "scrobbled":
            return root.scrobbleState === 3
                ? I18n.i18nc("@info:tooltip", "Last.fm: scrobbled")
                : I18n.i18nc("@info:tooltip", "Last.fm: sending the scrobble")
        case "queued":
            return root.uiBridge.lastFmScrobbleMessage.length > 0
                ? I18n.i18ncp("@info:tooltip %2 is the reason",
                    "Last.fm can't be reached (%2). %1 scrobble is waiting to be sent.",
                    "Last.fm can't be reached (%2). %1 scrobbles are waiting to be sent.",
                    root.pendingCount, root.uiBridge.lastFmScrobbleMessage)
                : I18n.i18ncp("@info:tooltip",
                    "Last.fm can't be reached. %1 scrobble is waiting to be sent.",
                    "Last.fm can't be reached. %1 scrobbles are waiting to be sent.",
                    root.pendingCount)
        case "failed":
            return root.uiBridge.lastFmScrobbleMessage.length > 0
                ? I18n.i18nc("@info:tooltip %1 is the reason", "Last.fm didn't accept the scrobble: %1",
                    root.uiBridge.lastFmScrobbleMessage)
                : I18n.i18nc("@info:tooltip", "Last.fm didn't accept the scrobble.")
        case "paused":
            return I18n.i18nc("@info:tooltip", "Last.fm: scrobbling is paused")
        default:
            return ""
        }
    }

    readonly property string glyphSource: {
        switch (root.look) {
        case "queued":
            return Qt.resolvedUrl("../assets/status-offline.svg")
        case "paused":
            return Qt.resolvedUrl("../assets/lastfm-paused.svg")
        default:
            return Qt.resolvedUrl("../assets/lastfm.svg")
        }
    }
    // The offline glyph is a 14 px one, centred where the Last.fm glyph is.
    readonly property int drawnGlyphSize: root.look === "queued"
        ? root.uiPalette.scaledSize(Kirigami.Units.iconSizes.small - 2)
        : root.glyphSize
    readonly property color glyphColor: {
        switch (root.look) {
        case "failed":
            return root.uiPalette.negative
        case "paused":
        case "notScrobbled":
            return root.uiPalette.textTertiary
        default:
            return root.uiPalette.textSecondary
        }
    }

    function togglePaused() {
        root.uiBridge.setLastFmScrobblingPaused(!root.paused)
    }

    function openMenu() {
        menu.popup(root, 0, root.height)
    }

    objectName: "scrobbleIndicator"
    // It appears when a track becomes eligible and leaves on its scrobble,
    // with no fade.
    visible: root.occupied
    implicitWidth: leftPadding + slotRow.implicitWidth + rightPadding
    // 22 px, or a little more than its words or its glyph and bar at a
    // larger font.
    implicitHeight: Math.max(22, Math.round(slotLabel.implicitHeight) + 4,
        root.glyphSize + root.barGap + root.barHeight)
    leftPadding: root.labelText.length > 0 ? 6 : 3
    rightPadding: 3
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.role: Accessible.Button
    Accessible.name: root.stateText
    Accessible.description: root.paused
        ? I18n.i18nc("@info", "Press to resume scrobbling.")
        : I18n.i18nc("@info", "Press to pause scrobbling.")

    // Keyboard focus shows the same tooltip as hover.
    ToolTip.visible: (root.hovered || root.visualFocus) && !menu.visible && root.stateText.length > 0
    ToolTip.delay: Kirigami.Units.toolTipDelay
    ToolTip.text: root.stateText

    onClicked: root.togglePaused()
    // Space presses the focused slot rather than playing or pausing; keys
    // with modifiers stay the shortcuts'.
    Keys.onShortcutOverride: function(event) {
        event.accepted = KeyUtils.isPlainSpace(event)
    }
    Keys.onReturnPressed: root.togglePaused()
    Keys.onEnterPressed: root.togglePaused()
    Keys.onMenuPressed: root.openMenu()
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
            root.openMenu()
            event.accepted = true
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: menu.popup()
    }

    Menu {
        id: menu
        objectName: "scrobbleIndicatorMenu"

        MenuItem {
            objectName: "scrobbleIndicatorPauseItem"
            text: I18n.i18nc("@action:inmenu", "Pause Scrobbling")
            checkable: true
            checked: root.paused
            onTriggered: root.uiBridge.setLastFmScrobblingPaused(checked)
        }
        MenuItem {
            objectName: "scrobbleIndicatorSettingsItem"
            text: I18n.i18nc("@action:inmenu", "Last.fm Settings…")
            onTriggered: root.settingsRequested()
        }
    }

    background: Rectangle {
        objectName: "scrobbleIndicatorBackground"
        radius: 3
        color: root.down
            ? root.uiPalette.pressed
            : (root.hovered ? root.uiPalette.hover : "transparent")
        border.width: root.visualFocus ? 2 : 0
        border.color: root.uiPalette.focusRing

        Behavior on color {
            enabled: Kirigami.Units.shortDuration > 0
            ColorAnimation {
                duration: Kirigami.Units.shortDuration
                easing.type: Easing.OutCubic
            }
        }
    }

    contentItem: Item {
        implicitWidth: slotRow.implicitWidth
        implicitHeight: slotRow.implicitHeight

        Row {
            id: slotRow
            anchors.verticalCenter: parent.verticalCenter
            spacing: root.labelText.length > 0 ? Kirigami.Units.largeSpacing : 0

            Label {
                id: slotLabel
                objectName: "scrobbleIndicatorLabel"
                anchors.verticalCenter: parent.verticalCenter
                visible: text.length > 0
                text: root.labelText
                font.pointSize: root.uiPalette.smallPointSize
                font.features: root.uiPalette.tabularFigures
                color: root.look === "queued" || root.look === "notScrobbled"
                    ? root.uiPalette.textSecondary
                    : root.uiPalette.text
                Accessible.ignored: true
            }

            // The glyph, and the bar's place under it, which is kept
            // when there is no bar, so that the glyph never moves.
            Item {
                objectName: "scrobbleIndicatorBox"
                anchors.verticalCenter: parent.verticalCenter
                width: root.glyphSize
                height: root.glyphSize + root.barGap + root.barHeight

                Kirigami.Icon {
                    objectName: "scrobbleIndicatorGlyph"
                    x: Math.round((root.glyphSize - width) / 2)
                    y: Math.round((root.glyphSize - height) / 2)
                    width: root.drawnGlyphSize
                    height: width
                    source: root.glyphSource
                    color: root.glyphColor
                    // Half strength while the scrobble is being sent.
                    opacity: root.look === "scrobbled" && !root.scrobbleAccepted ? 0.5 : 1
                    isMask: true
                }

                // The listening done, from the left; nothing drawn where it
                // is still to come.
                Rectangle {
                    objectName: "scrobbleIndicatorBar"
                    x: 0
                    y: root.glyphSize + root.barGap
                    width: root.barWidth * root.barProgress
                    height: root.barHeight
                    visible: root.barShown && width > 0
                    color: root.barColor
                }
            }
        }
    }
}
