// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Preferences

// The account, which connects and disconnects at once, and the scrobbling
// settings, which change on OK or Apply.
Preferences.PreferencesPage {
    id: root

    required property var uiBridge

    // 0: no account; 1: waiting for the user to allow Ferrous in the
    // browser; 2: connected.
    readonly property int authState: root.uiBridge.lastFmAuthState || 0
    readonly property bool connected: (root.uiBridge.lastFmUsername || "").length > 0
    readonly property bool buildConfigured: root.uiBridge.lastFmBuildConfigured === true
    // Which rows show. The rows' spacing follows these, not whether a row
    // shows: while the page is out of view no row does, and the page keeps
    // its height for Configure's.
    // Only a connected account has scrobbles to show.
    readonly property bool showsScrobbleProgress: root.authState === 2
    // The account line already says when the build has no Last.fm.
    readonly property bool showsStatus: (root.uiBridge.lastFmStatusText || "").length > 0 && root.buildConfigured
    readonly property bool showsPending: (root.uiBridge.lastFmPendingScrobbleCount || 0) > 0
    readonly property bool showsDropped: (root.uiBridge.lastFmDroppedScrobbleCount || 0) > 0

    title: I18n.i18nc("@title preferences page", "Last.fm")

    CheckBox {
        id: scrobblingCheckBox
        objectName: "lastFmScrobblingCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Scrobbling:")
        text: I18n.i18nc("@option:check", "Scrobble played tracks to Last.fm")
        // A build without Last.fm can't send a scrobble, as the account's
        // line says.
        enabled: root.buildConfigured
        checked: root.settings.value("lastFmScrobblingEnabled")
        onToggled: root.settings.setValue("lastFmScrobblingEnabled", checked)
    }

    Components.FormHelpText {
        objectName: "lastFmScrobblingHelp"
        uiPalette: root.uiPalette
        checkBox: scrobblingCheckBox
        // In a build without Last.fm it steps down with the setting, so the
        // account's line, which says why, leads (in the secondary colour it
        // outranked the disabled setting it explains).
        color: root.buildConfigured ? root.uiPalette.textSecondary : root.uiPalette.textTertiary
        // Without the status check box, it ends the scrobbling group.
        endsGroup: !root.showsScrobbleProgress
        text: I18n.i18nc("@info", "Tracks longer than 30 seconds are scrobbled when playback stops or the track "
            + "ends, once half of the track or 4 minutes has been played, whichever comes first.")
    }

    CheckBox {
        id: showScrobbleProgressCheckBox
        objectName: "lastFmShowScrobbleProgressCheckBox"
        visible: root.showsScrobbleProgress
        text: I18n.i18nc("@option:check", "Show the Last.fm status in the status bar")
        checked: root.settings.value("lastFmShowScrobbleProgress")
        onToggled: root.settings.setValue("lastFmShowScrobbleProgress", checked)
    }

    // The account is a group of its own, as General's groups are apart
    // (it stood 4 px under the scrobbling group's last row).
    Components.FormGroupGap {
        objectName: "lastFmAccountGap"
        uiPalette: root.uiPalette
        visible: root.showsScrobbleProgress
    }

    // The line's measure grows with the font (it wrapped at 432 px at
    // 24 pt, beside twice that free). What the form needs for it does not:
    // it still wraps where the page is narrow.
    Label {
        objectName: "lastFmAccountLabel"
        Kirigami.FormData.label: I18n.i18nc("@label", "Account:")
        Layout.fillWidth: true
        Layout.preferredWidth: Math.min(implicitWidth, Kirigami.Units.gridUnit * 24)
        Layout.maximumWidth: root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 24)
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: !root.buildConfigured
            ? I18n.i18nc("@info", "Last.fm isn’t available in this build.")
            : (root.connected
                ? I18n.i18nc("@info", "Connected as %1", root.uiBridge.lastFmUsername)
                : (root.authState === 1
                    ? I18n.i18nc("@info", "Allow Ferrous in the browser, then complete the connection.")
                    : I18n.i18nc("@info", "Not connected")))
    }

    // Only the buttons that apply now, their glyphs at the size of the
    // Library page's. Straight under the account, before its status, whose
    // lines come, go and wrap as tracks play and scrobbles are sent (under
    // them, Disconnect moved with each).
    RowLayout {
        id: accountButtons
        objectName: "lastFmAccountButtons"
        Layout.topMargin: Kirigami.Units.smallSpacing
        Layout.bottomMargin: Kirigami.Units.smallSpacing
        readonly property real glyphSize: root.uiPalette.labelIconSize
        spacing: Kirigami.Units.largeSpacing
        visible: root.buildConfigured

        Button {
            objectName: "lastFmConnectButton"
            visible: root.authState === 0 && !root.connected
            icon.width: accountButtons.glyphSize
            icon.height: accountButtons.glyphSize
            icon.name: "internet-services"
            text: I18n.i18nc("@action:button", "Connect…")
            onClicked: root.uiBridge.beginLastFmAuth()
        }

        Button {
            objectName: "lastFmCompleteButton"
            visible: root.authState === 1
            icon.width: accountButtons.glyphSize
            icon.height: accountButtons.glyphSize
            icon.name: "dialog-ok-apply"
            text: I18n.i18nc("@action:button", "Complete Connection")
            onClicked: root.uiBridge.completeLastFmAuth()
        }

        Button {
            objectName: "lastFmReconnectButton"
            visible: root.connected && root.authState !== 1
            icon.width: accountButtons.glyphSize
            icon.height: accountButtons.glyphSize
            icon.name: "view-refresh"
            text: I18n.i18nc("@action:button", "Reconnect…")
            onClicked: root.uiBridge.beginLastFmAuth()
        }

        Button {
            objectName: "lastFmDisconnectButton"
            visible: root.connected || root.authState !== 0
            icon.width: accountButtons.glyphSize
            icon.height: accountButtons.glyphSize
            icon.name: root.connected ? "network-disconnect" : "dialog-cancel"
            text: root.connected
                ? I18n.i18nc("@action:button", "Disconnect")
                : I18n.i18nc("@action:button", "Cancel Connection")
            onClicked: root.uiBridge.disconnectLastFm()
        }
    }

    Components.FormHelpText {
        id: statusLabel
        objectName: "lastFmStatusLabel"
        uiPalette: root.uiPalette
        // The scrobbles' lines go on from it, as one status.
        trailingSpace: root.showsPending || root.showsDropped
            ? 0
            : Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing - statusLabel.rowSpacing
        visible: root.showsStatus
        text: root.uiBridge.lastFmStatusText || ""
    }

    Components.FormHelpText {
        id: pendingLabel
        objectName: "lastFmPendingLabel"
        uiPalette: root.uiPalette
        // The dropped scrobbles' line goes on from it.
        trailingSpace: root.showsDropped
            ? 0
            : Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing - pendingLabel.rowSpacing
        visible: root.showsPending
        text: I18n.i18ncp("@info", "%1 scrobble waiting to be sent", "%1 scrobbles waiting to be sent",
            root.uiBridge.lastFmPendingScrobbleCount || 0)
    }

    // Scrobbles older than Last.fm accepts, or the oldest past the queue's
    // limit, are dropped unsent; this run's count. The diagnostics log says
    // which.
    Components.FormHelpText {
        id: droppedLabel
        objectName: "lastFmDroppedLabel"
        uiPalette: root.uiPalette
        visible: root.showsDropped
        text: I18n.i18ncp("@info", "%1 old scrobble was discarded from the queue",
            "%1 old scrobbles were discarded from the queue", root.uiBridge.lastFmDroppedScrobbleCount || 0)
    }
}
