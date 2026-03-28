// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

ScrollView {
    id: root

    required property var uiBridge
    required property var uiPalette

    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    ColumnLayout {
        width: root.availableWidth
        spacing: 0

        Components.SurfaceCard {
            Layout.fillWidth: true
            color: root.uiPalette.uiSurfaceColor
            borderColor: root.uiPalette.uiBorderColor
            implicitHeight: contentColumn.implicitHeight + 36

            ColumnLayout {
                id: contentColumn
                anchors.fill: parent
                anchors.margins: 18
                spacing: 14

                Label {
                    Layout.fillWidth: true
                    text: "Last.fm"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                CheckBox {
                    text: "Enable Last.fm scrobbling"
                    focusPolicy: Qt.NoFocus
                    checked: root.uiBridge.lastFmScrobblingEnabled
                    onToggled: root.uiBridge.setLastFmScrobblingEnabled(checked)
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    text: "Ferrous follows Last.fm's rule: only tracks longer than 30 seconds are eligible, and a scrobble is sent when playback stops or the track ends after at least half the track or 4 minutes has been listened, whichever comes first."
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: !root.uiBridge.lastFmBuildConfigured
                        ? "Last.fm is not configured in this build."
                        : (root.uiBridge.lastFmUsername.length > 0
                            ? "Connected account: " + root.uiBridge.lastFmUsername
                            : "No Last.fm account connected.")
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: root.uiBridge.lastFmStatusText.length > 0
                    color: Kirigami.Theme.disabledTextColor
                    text: root.uiBridge.lastFmStatusText
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.uiBridge.lastFmPendingScrobbleCount > 0
                    color: Kirigami.Theme.disabledTextColor
                    text: "Pending scrobbles: " + root.uiBridge.lastFmPendingScrobbleCount
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Button {
                        text: root.uiBridge.lastFmUsername.length > 0 ? "Reconnect" : "Connect"
                        enabled: root.uiBridge.lastFmBuildConfigured
                        onClicked: root.uiBridge.beginLastFmAuth()
                    }

                    Button {
                        text: "Complete Connection"
                        enabled: root.uiBridge.lastFmBuildConfigured && root.uiBridge.lastFmAuthState === 1
                        onClicked: root.uiBridge.completeLastFmAuth()
                    }

                    Button {
                        text: "Disconnect"
                        enabled: root.uiBridge.lastFmUsername.length > 0 || root.uiBridge.lastFmAuthState !== 0
                        onClicked: root.uiBridge.disconnectLastFm()
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
