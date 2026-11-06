// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils

// The status bar's Last.fm slot (#373): a fixed 16 px glyph box, with a
// few words before it when something needs the user's attention.
// - Listening: the Last.fm glyph, still, in the secondary colour; its
//   tooltip says where in the track it scrobbles.
// - Queued: "3 scrobbles waiting" and the offline glyph.
// - Failed: "Scrobble failed" and the glyph in the negative colour.
// - Paused by the user: the glyph in the tertiary colour.
// - Otherwise (scrobbled, sending, not eligible, signed out): nothing.
// Clicking it pauses or resumes scrobbling; its menu has the settings.
AbstractButton {
    id: root

    required property var uiBridge
    required property var uiPalette
    // The bridge's PlaybackClock, for where the track scrobbles; see
    // scrobbleAtSeconds. Without it, the listened time stands in for the
    // position.
    property var playbackClock: null

    // The Last.fm preferences were asked for.
    signal settingsRequested()

    readonly property bool shown: root.uiBridge.lastFmShowScrobbleProgress
        && root.uiBridge.lastFmScrobblingEnabled
        && root.uiBridge.lastFmAuthState === 2
    readonly property bool paused: root.uiBridge.lastFmScrobblingPaused
    readonly property int scrobbleState: root.uiBridge.lastFmScrobbleState
    readonly property int listenedMs: root.uiBridge.lastFmScrobbleListenedMs
    readonly property int thresholdMs: root.uiBridge.lastFmScrobbleThresholdMs
    readonly property int pendingCount: Math.max(0, Number(root.uiBridge.lastFmPendingScrobbleCount) || 0)

    // What the slot shows: "hidden", "listening", "queued", "failed",
    // "paused" or "empty". The scrobble states are
    // BinaryBridgeCodec::ScrobbleState.
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
        // Scrobbles wait while Last.fm can't be reached.
        if (root.pendingCount > 0) {
            return "queued"
        }
        if (root.scrobbleState === 1) {
            return "listening"
        }
        return "empty"
    }
    readonly property bool occupied: root.look !== "hidden" && root.look !== "empty"

    // Where in the track the play scrobbles, in seconds of the track: the
    // position plus the listening it still needs, or -1. While the track
    // plays, the position and the listened time advance together, so the
    // point stays where it is; it is counted again only where it moves: a
    // seek or a jump of the position (paused or not), a new track, a
    // replay of the same track (Repeat One), and the scrobble point itself.
    property real scrobbleAtSeconds: -1
    // The listened time of the latest snapshot: one that goes back starts
    // a new play, whose count the scrobbler may send after the position's
    // jump.
    property int lastListenedMs: 0

    function refreshScrobbleAt() {
        if (root.look !== "listening" || root.thresholdMs <= 0) {
            root.scrobbleAtSeconds = -1
            return
        }
        const remaining = Math.max(0, root.thresholdMs - root.listenedMs) / 1000
        const position = root.playbackClock
            ? Number(root.playbackClock.currentPositionSeconds())
            : root.listenedMs / 1000
        root.scrobbleAtSeconds = (isFinite(position) ? Math.max(0, position) : 0) + remaining
    }

    onLookChanged: root.refreshScrobbleAt()
    onThresholdMsChanged: root.refreshScrobbleAt()
    onListenedMsChanged: {
        const newPlay = root.listenedMs < root.lastListenedMs
        root.lastListenedMs = root.listenedMs
        if (newPlay) {
            root.refreshScrobbleAt()
        }
    }
    Component.onCompleted: {
        root.lastListenedMs = root.listenedMs
        root.refreshScrobbleAt()
    }

    Connections {
        target: root.playbackClock || null
        function onPositionJumped() {
            root.refreshScrobbleAt()
        }
    }

    Connections {
        target: root.uiBridge
        ignoreUnknownSignals: true
        function onTrackIdentityChanged() {
            root.refreshScrobbleAt()
        }
    }

    // The slot's words, before the glyph.
    readonly property string labelText: {
        switch (root.look) {
        case "queued":
            return I18n.i18ncp("@info:status", "%1 scrobble waiting", "%1 scrobbles waiting",
                root.pendingCount)
        case "failed":
            return I18n.i18nc("@info:status", "Scrobble failed")
        default:
            return ""
        }
    }

    // What the slot says: its tooltip and accessible name.
    readonly property string stateText: {
        switch (root.look) {
        case "listening":
            return root.scrobbleAtSeconds >= 0
                ? I18n.i18nc("@info:tooltip %1 is a time in the track", "Last.fm: scrobbles at %1",
                    FormatUtils.formatClockTime(Math.round(root.scrobbleAtSeconds)))
                : I18n.i18nc("@info:tooltip", "Last.fm: listening")
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

    readonly property string glyphSource: root.look === "queued"
        ? Qt.resolvedUrl("../assets/status-offline.svg")
        : Qt.resolvedUrl("../assets/lastfm.svg")
    readonly property color glyphColor: {
        switch (root.look) {
        case "failed":
            return root.uiPalette.negative
        case "paused":
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
    implicitHeight: 22
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
                objectName: "scrobbleIndicatorLabel"
                anchors.verticalCenter: parent.verticalCenter
                visible: text.length > 0
                text: root.labelText
                font.pointSize: root.uiPalette.smallPointSize
                font.features: root.uiPalette.tabularFigures
                color: root.look === "queued" ? root.uiPalette.textSecondary : root.uiPalette.text
                Accessible.ignored: true
            }

            // The fixed 16 px glyph box.
            Item {
                anchors.verticalCenter: parent.verticalCenter
                width: Kirigami.Units.iconSizes.small
                height: Kirigami.Units.iconSizes.small

                Kirigami.Icon {
                    objectName: "scrobbleIndicatorGlyph"
                    anchors.centerIn: parent
                    // The offline glyph is a 14 px one in the 16 px box.
                    width: root.look === "queued" ? Kirigami.Units.iconSizes.small - 2 : parent.width
                    height: width
                    source: root.glyphSource
                    color: root.glyphColor
                    isMask: true
                }
            }
        }
    }
}
