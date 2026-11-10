// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Shapes 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils

// The status bar's Last.fm slot (#373, #470): a fixed 20 px box, with a
// few words before it when something needs saying.
// - Listening: the Last.fm glyph in a ring that fills with the listening
//   done towards the scrobble; its tooltip says how much is left. Both
//   come from the scrobbler's listened time, which a seek doesn't move
//   and a pause holds; a new play (a new track, a replay, Repeat One)
//   starts it over.
// - Not scrobbled: once playback has stopped, by the end of the queue or
//   a stop, after a play that ended before its scrobble point. "Not
//   scrobbled", the empty ring and the glyph in the tertiary colour; the
//   tooltip says how far the play got. A seek never decides it: the
//   countdown goes on while the track plays.
// - Being sent: the ring closed, from the moment the play reaches its
//   scrobble point; the send goes on in the background.
// - Scrobbled: once Last.fm has accepted it, the closed ring and the glyph
//   in the text colour. A ring with no gap is the end of the listening
//   ring, apart from one nearly full by its shape; no fill, as a routine
//   success is no louder than the rest of the status bar.
// - Queued: "3 scrobbles waiting" and the offline glyph, only while
//   scrobbles stay queued: after a send failed, or while Last.fm holds
//   them back.
// - Failed: "Scrobble failed", and the closed ring and the glyph in the
//   negative colour: the slot's only colour.
// - Paused by the user: the glyph struck through, in the tertiary colour,
//   without the ring.
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
        // ring closes at once, and the slot says it waits only if the send
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

    // The ring around the glyph: its radius in the box, and how much of it
    // is drawn, 0 to 1. The share moves by whole pixels of the arc too,
    // so a long scrobble point redraws it more rarely still.
    readonly property bool ringShown: root.look === "listening" || root.look === "notScrobbled"
        || root.look === "scrobbled" || root.look === "failed"
    readonly property real ringStroke: 1.5
    readonly property real ringRadius: (root.boxSize - root.ringStroke) / 2
    readonly property real ringProgress: {
        if (root.look === "scrobbled" || root.look === "failed") {
            return 1
        }
        if (root.look !== "listening" || root.thresholdSeconds <= 0) {
            return 0
        }
        const steps = Math.max(1, Math.round(2 * Math.PI * root.ringRadius * Math.max(1, Screen.devicePixelRatio)))
        return Math.floor(Math.min(1, root.listenedSeconds / root.thresholdSeconds) * steps) / steps
    }
    // Last.fm accepted the scrobble.
    readonly property bool scrobbleAccepted: root.look === "scrobbled" && root.scrobbleState === 3
    readonly property color ringColor: {
        if (root.look === "failed") {
            return root.uiPalette.negative
        }
        return root.scrobbleAccepted ? root.uiPalette.text : root.uiPalette.textSecondary
    }
    // The ring's track, where the listening is still to come: opaque, the
    // least of the arc's colour over the status bar that is 3:1 on it, so
    // that the empty ring reads (WCAG 1.4.11) and stays quieter than the arc.
    readonly property color ringTrackColor: ColorUtils.mixColor(root.uiPalette.chrome,
        root.uiPalette.textSecondary,
        ColorUtils.opacityForContrast(root.uiPalette.textSecondary, root.uiPalette.chrome, 0.3, 3.05))

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

    // The box the glyph and its ring are drawn in.
    readonly property int boxSize: Kirigami.Units.iconSizes.small + 4
    readonly property string glyphSource: {
        switch (root.look) {
        case "queued":
            return Qt.resolvedUrl("../assets/status-offline.svg")
        case "paused":
            return Qt.resolvedUrl("../assets/lastfm-paused.svg")
        default:
            return root.ringShown
                ? Qt.resolvedUrl("../assets/lastfm-ring.svg")
                : Qt.resolvedUrl("../assets/lastfm.svg")
        }
    }
    // Inside the ring, a 14 px glyph with a heavier stroke; the offline
    // glyph is a 14 px one too.
    readonly property int glyphSize: root.ringShown || root.look === "queued"
        ? Kirigami.Units.iconSizes.small - 2
        : Kirigami.Units.iconSizes.small
    readonly property color glyphColor: {
        if (root.scrobbleAccepted) {
            return root.uiPalette.text
        }
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
    // 22 px, or a little more than its words at a larger font.
    implicitHeight: Math.max(22, Math.round(slotLabel.implicitHeight) + 4)
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
            ? ColorUtils.withAlpha(root.uiPalette.text, 0.10)
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

            // The fixed glyph box.
            Item {
                objectName: "scrobbleIndicatorBox"
                anchors.verticalCenter: parent.verticalCenter
                width: root.boxSize
                height: root.boxSize

                // The ring: its track, and the listening done on it from
                // the top, clockwise.
                Shape {
                    objectName: "scrobbleIndicatorRing"
                    anchors.fill: parent
                    visible: root.ringShown
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        objectName: "scrobbleIndicatorRingTrack"
                        strokeColor: root.ringProgress < 1 ? root.ringTrackColor : "transparent"
                        strokeWidth: root.ringStroke
                        fillColor: "transparent"
                        PathAngleArc {
                            centerX: root.boxSize / 2
                            centerY: root.boxSize / 2
                            radiusX: root.ringRadius
                            radiusY: root.ringRadius
                            startAngle: 0
                            sweepAngle: 360
                        }
                    }
                    ShapePath {
                        objectName: "scrobbleIndicatorRingArc"
                        strokeColor: root.ringProgress > 0 ? root.ringColor : "transparent"
                        strokeWidth: root.ringStroke
                        fillColor: "transparent"
                        capStyle: ShapePath.FlatCap
                        PathAngleArc {
                            centerX: root.boxSize / 2
                            centerY: root.boxSize / 2
                            radiusX: root.ringRadius
                            radiusY: root.ringRadius
                            startAngle: -90
                            sweepAngle: 360 * root.ringProgress
                        }
                    }
                }

                Kirigami.Icon {
                    objectName: "scrobbleIndicatorGlyph"
                    anchors.centerIn: parent
                    width: root.glyphSize
                    height: width
                    source: root.glyphSource
                    color: root.glyphColor
                    isMask: true
                }
            }
        }
    }
}
