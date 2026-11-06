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

    title: I18n.i18nc("@title preferences page", "Last.fm")

    CheckBox {
        id: scrobblingCheckBox
        objectName: "lastFmScrobblingCheckBox"
        Kirigami.FormData.label: I18n.i18nc("@label", "Scrobbling:")
        text: I18n.i18nc("@option:check", "Scrobble played tracks to Last.fm")
        checked: root.settings.value("lastFmScrobblingEnabled")
        onToggled: root.settings.setValue("lastFmScrobblingEnabled", checked)
    }

    Components.FormHelpText {
        uiPalette: root.uiPalette
        indent: scrobblingCheckBox.contentItem.x
        text: I18n.i18nc("@info", "Tracks longer than 30 seconds are scrobbled when playback stops or the track "
            + "ends, once half of the track or 4 minutes has been played, whichever comes first.")
    }

    CheckBox {
        objectName: "lastFmShowScrobbleProgressCheckBox"
        // Only a connected account has scrobbles to show.
        visible: root.authState === 2
        text: I18n.i18nc("@option:check", "Show the Last.fm status in the status bar")
        checked: root.settings.value("lastFmShowScrobbleProgress")
        onToggled: root.settings.setValue("lastFmShowScrobbleProgress", checked)
    }

    Label {
        objectName: "lastFmAccountLabel"
        Kirigami.FormData.label: I18n.i18nc("@label", "Account:")
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        wrapMode: Text.Wrap
        text: !root.buildConfigured
            ? I18n.i18nc("@info", "Last.fm isn’t available in this build.")
            : (root.connected
                ? I18n.i18nc("@info", "Connected as %1", root.uiBridge.lastFmUsername)
                : (root.authState === 1
                    ? I18n.i18nc("@info", "Allow Ferrous in the browser, then complete the connection.")
                    : I18n.i18nc("@info", "Not connected")))
    }

    Components.FormHelpText {
        objectName: "lastFmStatusLabel"
        uiPalette: root.uiPalette
        // The account line already says when the build has no Last.fm.
        visible: text.length > 0 && root.buildConfigured
        text: root.uiBridge.lastFmStatusText || ""
    }

    Components.FormHelpText {
        objectName: "lastFmPendingLabel"
        uiPalette: root.uiPalette
        visible: (root.uiBridge.lastFmPendingScrobbleCount || 0) > 0
        text: I18n.i18ncp("@info", "%1 scrobble waiting to be sent", "%1 scrobbles waiting to be sent",
            root.uiBridge.lastFmPendingScrobbleCount || 0)
    }

    // Only the buttons that apply now.
    RowLayout {
        spacing: Kirigami.Units.largeSpacing
        visible: root.buildConfigured

        Button {
            objectName: "lastFmConnectButton"
            visible: root.authState === 0 && !root.connected
            icon.name: "internet-services"
            text: I18n.i18nc("@action:button", "Connect…")
            onClicked: root.uiBridge.beginLastFmAuth()
        }

        Button {
            objectName: "lastFmCompleteButton"
            visible: root.authState === 1
            text: I18n.i18nc("@action:button", "Complete Connection")
            onClicked: root.uiBridge.completeLastFmAuth()
        }

        Button {
            objectName: "lastFmReconnectButton"
            visible: root.connected && root.authState !== 1
            text: I18n.i18nc("@action:button", "Reconnect…")
            onClicked: root.uiBridge.beginLastFmAuth()
        }

        Button {
            objectName: "lastFmDisconnectButton"
            visible: root.connected || root.authState !== 0
            text: root.connected
                ? I18n.i18nc("@action:button", "Disconnect")
                : I18n.i18nc("@action:button", "Cancel Connection")
            onClicked: root.uiBridge.disconnectLastFm()
        }
    }
}
