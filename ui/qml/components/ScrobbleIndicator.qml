// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils

// The status bar's Last.fm slot (#373, #470): the Last.fm glyph, with a
// short bar beneath it, and a few words before it when something needs
// saying (a 10 px glyph inside a ring was a smudge at 1x, and its states
// looked alike).
// The glyph's ink is centred on the slot, on whole pixels (with the bar's
// place under it, the glyph sat 2.5 px above the status bar's middle). Its
// glyphs are one family (a grid of squares for the "as", a staircase for
// its strike and thin antialiased strokes for offline read as three
// hands): outlines drawn for 16 px with their straight edges on whole
// pixels and their corners as arcs, all 16 px.
// The bar is the glyph's: as wide as the glyph, 3 px tall and 2 px under
// its ink, with square ends (at 2 px its proportions did not read at 1x).
// It is the listening done towards the scrobble, in the glyph's colour,
// the secondary text colour (at least 3:1 on the status bar), over an
// opaque trough at 3:1 on the status bar, so that it reads as a share of
// a whole (a bar alone, 6 to 14 px long and brighter than its glyph, did
// not read as progress). It is a fifth of the scan bar's length, with
// square ends where the scan bar's are round, and the scan bar has its
// percent beside it (the scan bar is 72 by 4 px), so that it does not
// read as a second scan or a download.
// - Listening: the glyph in the secondary colour, and the bar filling its
//   trough with the listening done towards the scrobble, from a short mark
//   at the left; the tooltip says how much is left. Both come from the scrobbler's listened
//   time, which a seek doesn't move and a pause holds; a new play (a new
//   track, a replay, Repeat One) starts it over. Listening always has the
//   trough and the mark and scrobbled has neither, so the two differ
//   before any listening too.
// - Not scrobbled: once playback has stopped, by the end of the queue or
//   a stop, after a play that ended before its scrobble point. "Not
//   scrobbled" and the glyph in the tertiary colour, without the bar; the
//   tooltip says how far the play got. A seek never decides it: the
//   countdown goes on while the track plays.
// - Being sent: the bar full, from the moment the play reaches its
//   scrobble point, while the send goes on in the background. Only then:
//   listening stops short of full.
// - Scrobbled: once Last.fm has accepted it, the glyph alone in the
//   secondary colour, so that the routine success is no louder than the
//   status bar's other words.
// - Queued: "3 scrobbles waiting" and the offline glyph, only while
//   scrobbles stay queued: after a send failed, or while Last.fm holds
//   them back.
// - Failed: "Scrobble failed" in the text colour and the glyph in the
//   negative colour, without the bar: the status bar's one treatment of an
//   error, as its new issues.
// - Paused by the user: the glyph struck through (#470), its letters in
//   the tertiary colour and the strike over them in the text colour, in a
//   clear casing, without the bar (in one colour the strike's clear space
//   took so much of the letters that neither read at 1x).
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
    // The glyph's ink, in its 16 px drawing: columns 1 to 14 and rows 3 to
    // 12, centred in it.
    readonly property int inkBottom: Math.round(root.glyphSize * 13 / 16)
    // The bar's trough under it: the glyph's width, 3 px tall and 2 px
    // below the ink at the default font, a little more with a larger one,
    // within the status bar's height: the status bar grows with the small
    // font's line, which grows more slowly than the glyph.
    readonly property int barWidth: root.glyphSize
    readonly property int barHeight: Math.max(3, Math.round(root.glyphSize * 3 / 20))
    readonly property int barGap: Math.max(2, Math.round(root.glyphSize / 10))
    // The bar's top, from the glyph's.
    readonly property int barY: root.inkBottom + root.barGap
    // The slot's height when nothing in it needs more: the status bar sets
    // it to its controls' height, so the slot is centred in the bar on whole
    // pixels and the glyph on the slot.
    property int blockHeight: 22
    // From the slot's top up to the status bar's, which the keyboard's menu
    // stays above: the status bar sets it.
    property real roomToBarTop: 0
    // Last.fm accepted the scrobble.
    readonly property bool scrobbleAccepted: root.look === "scrobbled" && root.scrobbleState === 3
    // The trough and the bar in it: while listening, and while the scrobble
    // is being sent.
    readonly property bool barShown: root.look === "listening"
        || (root.look === "scrobbled" && !root.scrobbleAccepted)
    // How much of the trough the bar fills, 0 to 1: the listening done, by
    // whole device pixels of the trough, so a long scrobble point redraws
    // it more rarely still; full once the play has reached its scrobble
    // point.
    readonly property real barProgress: {
        if (root.look === "scrobbled") {
            return root.barShown ? 1 : 0
        }
        if (root.look !== "listening" || root.thresholdSeconds <= 0) {
            return 0
        }
        const steps = Math.max(1, Math.round(root.barWidth * Math.max(1, Screen.devicePixelRatio)))
        return Math.floor(Math.min(1, root.listenedSeconds / root.thresholdSeconds) * steps) / steps
    }
    // The bar: the secondary text colour, the glyph's. Its trough: the text
    // at 35% in the status bar, opaque, and darkened or lightened where
    // that is under 3:1 on it, so that the whole reads as a whole (the text
    // at 20% was 1.9:1 / 1.5:1), and still well short of the bar: #6e7173 /
    // #86888b in Breeze.
    readonly property color barColor: root.uiPalette.textSecondary
    readonly property color troughColor: root.uiPalette.readableOn(
        ColorUtils.mixColor(root.uiPalette.chrome, root.uiPalette.text, 0.35),
        [root.uiPalette.chrome], 3.1)
    // While listening, the bar is never less than a short mark from the
    // left, so that listening that has just started reads apart from a
    // scrobble, which has no bar (at 0% it was the trough alone); and it
    // stops as short a mark before the trough's end, so that only a decided
    // scrobble fills it (the last of the listening looked like sending).
    readonly property int minimumBarLength: Math.max(3, Math.round(root.glyphSize * 3 / 16))
    readonly property real barLength: root.look === "listening"
        ? Math.max(root.minimumBarLength,
            Math.min(root.barWidth - root.minimumBarLength, root.barWidth * root.barProgress))
        : root.barWidth * root.barProgress

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

    // From the keyboard the menu opens in the window, above the status bar
    // and on the slot's right edge (under the slot, it was under the bar
    // and out of the window). It is an item of the window then: Wayland's
    // compositor places a window popup under its anchor wherever that is
    // in the window.
    function openMenu() {
        menu.popupType = Popup.Item
        menu.popup(keyboardMenuAnchor, 0, 0)
    }

    function openMenuAt(x, y) {
        menu.popupType = Popup.Window
        menu.popupAt(root, x, y)
    }

    // Where that menu's top left is: it follows the menu's size, which
    // Breeze gives it from its items as they show.
    Item {
        id: keyboardMenuAnchor
        visible: false
        x: root.width - menu.width
        y: -root.roomToBarTop - menu.height - Kirigami.Units.smallSpacing
    }

    objectName: "scrobbleIndicator"
    // It appears when a track becomes eligible and leaves on its scrobble,
    // with no fade.
    visible: root.occupied
    implicitWidth: leftPadding + slotRow.implicitWidth + rightPadding
    // The block's height, or a little more than its words, or than the
    // glyph centred with the bar under it, at a larger font.
    implicitHeight: Math.max(root.blockHeight, Math.round(slotLabel.implicitHeight) + 4,
        2 * (root.barY + root.barHeight) - root.glyphSize)
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
        onTapped: function(eventPoint) { root.openMenuAt(eventPoint.position.x, eventPoint.position.y) }
    }

    Components.AppMenu {
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
            height: parent.height
            spacing: root.labelText.length > 0 ? Kirigami.Units.largeSpacing : 0

            Label {
                id: slotLabel
                objectName: "scrobbleIndicatorLabel"
                anchors.verticalCenter: parent.verticalCenter
                visible: text.length > 0
                textFormat: Text.PlainText
                text: root.labelText
                font.pointSize: root.uiPalette.smallPointSize
                font.features: root.uiPalette.tabularFigures
                color: root.look === "queued" || root.look === "notScrobbled"
                    ? root.uiPalette.textSecondary
                    : root.uiPalette.text
                Accessible.ignored: true
            }

            // The glyph's cell, centred on the slot on whole pixels, with
            // the bar hanging under its ink.
            Item {
                objectName: "scrobbleIndicatorBox"
                y: Math.round((parent.height - height) / 2)
                width: root.glyphSize
                height: root.glyphSize

                Kirigami.Icon {
                    objectName: "scrobbleIndicatorGlyph"
                    width: root.glyphSize
                    height: width
                    // At its size, as wide as the bar under it at a large
                    // font, not rounded down to a theme icon size.
                    roundToIconSize: false
                    source: root.glyphSource
                    color: root.glyphColor
                    isMask: true
                }

                // Paused: the strike, apart from the letters it crosses.
                Kirigami.Icon {
                    objectName: "scrobbleIndicatorStrike"
                    visible: root.look === "paused"
                    width: root.glyphSize
                    height: width
                    roundToIconSize: false
                    source: Qt.resolvedUrl("../assets/lastfm-strike.svg")
                    color: root.uiPalette.text
                    isMask: true
                }

                // The whole the listening is a share of.
                Rectangle {
                    objectName: "scrobbleIndicatorTrough"
                    y: root.barY
                    width: root.barWidth
                    height: root.barHeight
                    visible: root.barShown
                    color: root.troughColor

                    // The listening done, from the left.
                    Rectangle {
                        objectName: "scrobbleIndicatorBar"
                        width: root.barLength
                        height: parent.height
                        visible: width > 0
                        color: root.barColor
                    }
                }
            }
        }
    }
}
