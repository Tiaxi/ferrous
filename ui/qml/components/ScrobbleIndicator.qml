// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils

// The status bar's Last.fm scrobble progress: a ring around a music note
// that fills clockwise from 12 o'clock with the time listened, as the
// scrobbler counts it, towards the scrobble point. Its colour tells what
// became of the scrobble. Clicking it pauses or resumes scrobbling.
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
    readonly property int listenedMs: root.uiBridge.lastFmScrobbleListenedMs
    readonly property int thresholdMs: root.uiBridge.lastFmScrobbleThresholdMs

    // What the ring shows: "idle", "filling", "accepted", "failed",
    // "waiting", "ineligible" or "paused". The scrobble states are
    // BinaryBridgeCodec::ScrobbleState.
    readonly property string look: {
        if (root.paused || root.scrobbleState === 7) {
            return "paused"
        }
        switch (root.scrobbleState) {
        case 1:
        case 2:
            return "filling"
        case 3:
            return "accepted"
        case 4:
            return "failed"
        case 5:
            return "waiting"
        case 6:
            return "ineligible"
        default:
            return "idle"
        }
    }
    // How much of the ring is filled, 0 to 1.
    readonly property real progress: {
        switch (root.look) {
        case "filling":
            if (root.scrobbleState === 2) {
                return 1
            }
            return root.thresholdMs > 0
                ? Math.max(0, Math.min(1, root.listenedMs / root.thresholdMs))
                : 0
        case "accepted":
        case "failed":
        case "waiting":
            return 1
        default:
            return 0
        }
    }
    readonly property color ringColor: {
        switch (root.look) {
        case "filling":
            return Kirigami.Theme.highlightColor
        case "accepted":
            return Kirigami.Theme.positiveTextColor
        case "failed":
            return Kirigami.Theme.neutralTextColor
        default:
            return root.uiPalette.uiMutedTextColor
        }
    }
    readonly property bool muted: root.look !== "filling"
        && root.look !== "accepted"
        && root.look !== "failed"
    // The badge in the corner: "clock", "pause" or none.
    readonly property string badge: root.look === "waiting"
        ? "clock"
        : (root.look === "paused" ? "pause" : "")

    readonly property real devicePixelRatio: Screen.devicePixelRatio
    readonly property real ringLineWidth: 2
    readonly property real ringRadius: (Math.min(ring.width, ring.height) - root.ringLineWidth) / 2 - 1
    // The arc grows by a device pixel a step: a smaller change of the
    // progress draws the same ring.
    readonly property int progressSteps: Math.max(
        12, Math.ceil(2 * Math.PI * Math.max(1, root.ringRadius) * root.devicePixelRatio))
    readonly property int progressStep: Math.floor(root.progress * root.progressSteps)
    // Everything the ring is drawn from; it is drawn again only when this
    // changes, not with every snapshot of the listened time.
    readonly property string drawKey: [
        root.look, root.progressStep, root.progressSteps, root.ringColor,
        root.uiPalette.uiTextColor, root.uiPalette.uiMutedTextColor,
        root.uiPalette.uiPaneColor, ring.width, ring.height, root.devicePixelRatio
    ].join("|")
    // How many times the ring was drawn.
    readonly property alias paintCount: ring.paintCount

    function stateDescription() {
        switch (root.look) {
        case "paused":
            return root.paused
                ? I18n.i18nc("@info:tooltip", "Scrobbling is paused. This track won't be scrobbled.")
                : I18n.i18nc("@info:tooltip", "Scrobbling was paused while this track played, so it won't be scrobbled. Scrobbling resumes with the next track.")
        case "filling":
            if (root.scrobbleState === 2) {
                return I18n.i18nc("@info:tooltip", "Sending the scrobble to Last.fm…")
            }
            return I18n.i18nc("@info:tooltip", "Listening towards the scrobble point.")
        case "accepted":
            return I18n.i18nc("@info:tooltip", "Scrobbled to Last.fm.")
        case "failed":
            return root.uiBridge.lastFmScrobbleMessage.length > 0
                ? I18n.i18nc("@info:tooltip %1 is the reason", "The scrobble failed: %1",
                    root.uiBridge.lastFmScrobbleMessage)
                : I18n.i18nc("@info:tooltip", "The scrobble failed.")
        case "waiting":
            return root.uiBridge.lastFmScrobbleMessage.length > 0
                ? I18n.i18nc("@info:tooltip %1 is the reason",
                    "Last.fm can't be reached (%1). The scrobble is queued and sent later.",
                    root.uiBridge.lastFmScrobbleMessage)
                : I18n.i18nc("@info:tooltip", "Last.fm can't be reached. The scrobble is queued and sent later.")
        case "ineligible":
            return root.uiBridge.lastFmScrobbleIneligibility === 2
                ? I18n.i18nc("@info:tooltip", "Tracks without an artist and a title aren't scrobbled.")
                : I18n.i18nc("@info:tooltip", "Tracks of 30 seconds or less aren't scrobbled.")
        default:
            return I18n.i18nc("@info:tooltip", "Nothing is playing.")
        }
    }

    // The tooltip's first line: the state, with the time left while
    // listening, which only a shown tooltip follows.
    function tooltipStateText() {
        if (root.look === "filling" && root.scrobbleState === 1 && root.thresholdMs > 0) {
            const secondsLeft = Math.max(0, Math.ceil((root.thresholdMs - root.listenedMs) / 1000))
            return I18n.i18nc("@info:tooltip %1 is a duration", "Scrobbles to Last.fm in %1.",
                FormatUtils.formatSeekTime(secondsLeft))
        }
        return root.stateDescription()
    }

    function togglePaused() {
        root.uiBridge.setLastFmScrobblingPaused(!root.paused)
    }

    function openMenu() {
        menu.popup(root, 0, root.height)
    }

    objectName: "scrobbleIndicator"
    visible: root.shown
    implicitWidth: 24
    implicitHeight: 20
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.role: Accessible.Button
    Accessible.name: I18n.i18nc("@action:button", "Last.fm scrobble progress")
    Accessible.description: root.stateDescription() + " " + (root.paused
        ? I18n.i18nc("@info", "Press to resume scrobbling.")
        : I18n.i18nc("@info", "Press to pause scrobbling."))

    ToolTip.visible: (root.hovered || root.visualFocus) && !menu.visible
    ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
    ToolTip.text: root.ToolTip.visible
        ? root.tooltipStateText() + "\n" + (root.paused
            ? I18n.i18nc("@info:tooltip", "Click to resume scrobbling.")
            : I18n.i18nc("@info:tooltip", "Click to pause scrobbling."))
        : ""

    onClicked: root.togglePaused()
    // Space presses the focused ring rather than playing or pausing; keys
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
        radius: Kirigami.Units.cornerRadius
        color: root.down
            ? Qt.rgba(root.uiPalette.uiTextColor.r, root.uiPalette.uiTextColor.g,
                root.uiPalette.uiTextColor.b, 0.16)
            : (root.hovered
                ? Qt.rgba(root.uiPalette.uiTextColor.r, root.uiPalette.uiTextColor.g,
                    root.uiPalette.uiTextColor.b, 0.08)
                : "transparent")
        border.width: root.visualFocus ? 1 : 0
        border.color: Kirigami.Theme.focusColor
    }

    contentItem: Canvas {
        id: ring
        objectName: "scrobbleIndicatorRing"

        property int paintCount: 0

        implicitWidth: 20
        implicitHeight: 20

        onPaint: {
            paintCount += 1
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const cx = w / 2
            const cy = h / 2
            const r = root.ringRadius
            if (r <= 0) {
                return
            }
            const start = -Math.PI / 2
            const muted = root.uiPalette.uiMutedTextColor
            const glyphColor = root.muted ? muted : root.uiPalette.uiTextColor

            // The track of the ring: dashed for a play that cannot be
            // scrobbled.
            ctx.lineWidth = root.ringLineWidth
            ctx.lineCap = "butt"
            ctx.strokeStyle = Qt.rgba(muted.r, muted.g, muted.b, root.look === "ineligible" ? 0.9 : 0.35)
            if (root.look === "ineligible") {
                const dashes = 10
                const span = 2 * Math.PI / dashes
                for (let i = 0; i < dashes; ++i) {
                    ctx.beginPath()
                    ctx.arc(cx, cy, r, start + i * span, start + (i + 0.55) * span, false)
                    ctx.stroke()
                }
            } else {
                ctx.beginPath()
                ctx.arc(cx, cy, r, 0, 2 * Math.PI, false)
                ctx.stroke()
            }

            // The progress, clockwise from 12 o'clock.
            const filled = root.progressStep / root.progressSteps
            if (filled > 0) {
                ctx.strokeStyle = root.ringColor
                ctx.beginPath()
                ctx.arc(cx, cy, r, start, start + filled * 2 * Math.PI, false)
                ctx.stroke()
            }

            // A music note: a slanted head and a stem with a flag.
            const unit = r / 7
            const headX = cx - 1.4 * unit
            const headY = cy + 2.4 * unit
            ctx.fillStyle = glyphColor
            ctx.strokeStyle = glyphColor
            ctx.save()
            ctx.translate(headX, headY)
            ctx.rotate(-0.45)
            ctx.scale(1, 0.72)
            ctx.beginPath()
            ctx.arc(0, 0, 1.9 * unit, 0, 2 * Math.PI, false)
            ctx.fill()
            ctx.restore()
            const stemX = headX + 1.7 * unit
            const stemTop = cy - 4.2 * unit
            ctx.lineWidth = Math.max(1, 0.9 * unit)
            ctx.beginPath()
            ctx.moveTo(stemX, headY - 0.6 * unit)
            ctx.lineTo(stemX, stemTop)
            ctx.quadraticCurveTo(stemX + 2.6 * unit, stemTop + 1.2 * unit, stemX + 2.4 * unit, stemTop + 3.6 * unit)
            ctx.stroke()

            if (root.look === "paused") {
                // A slash across the note.
                ctx.strokeStyle = muted
                ctx.lineWidth = 1.5
                ctx.beginPath()
                ctx.moveTo(cx + r * 0.62, cy - r * 0.62)
                ctx.lineTo(cx - r * 0.62, cy + r * 0.62)
                ctx.stroke()
            }

            if (root.badge.length > 0) {
                // A small badge over the bottom right of the ring.
                const br = Math.max(3.5, r * 0.5)
                const bx = w - br - 0.5
                const by = h - br - 0.5
                ctx.fillStyle = root.uiPalette.uiPaneColor
                ctx.beginPath()
                ctx.arc(bx, by, br + 1, 0, 2 * Math.PI, false)
                ctx.fill()
                ctx.strokeStyle = muted
                ctx.fillStyle = muted
                ctx.lineWidth = 1
                if (root.badge === "clock") {
                    ctx.beginPath()
                    ctx.arc(bx, by, br - 0.5, 0, 2 * Math.PI, false)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(bx, by - br * 0.6)
                    ctx.lineTo(bx, by)
                    ctx.lineTo(bx + br * 0.5, by)
                    ctx.stroke()
                } else {
                    const barWidth = Math.max(1, br * 0.42)
                    const barHeight = br * 1.3
                    ctx.fillRect(bx - barWidth * 1.25, by - barHeight / 2, barWidth, barHeight)
                    ctx.fillRect(bx + barWidth * 0.25, by - barHeight / 2, barWidth, barHeight)
                }
            }
        }
    }

    onDrawKeyChanged: ring.requestPaint()
}
