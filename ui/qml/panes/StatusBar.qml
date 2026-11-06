// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/ErrorUtils.js" as ErrorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../components" as Components

ToolBar {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var channelStatusIconSource
    required property bool themeIsDark
    // The app's ErrorController; the status bar shows how many errors it
    // keeps, and stands out until the user has seen the newest.
    property var errorController: null
    property string positionText: "00:00"

    // The scrobble progress indicator asked for the Last.fm preferences.
    signal lastFmSettingsRequested()

    implicitHeight: Math.max(contentItem.implicitHeight, 20) + topPadding + bottomPadding
    leftPadding: 14
    rightPadding: 10
    topPadding: 2
    bottomPadding: 2

    // Chrome, with the last of the main window's four seams along its top.
    background: Rectangle {
        color: root.uiPalette.chrome
        Rectangle {
            objectName: "statusBarSeam"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: root.uiPalette.chromeLine
        }
    }

    readonly property int errorCount: root.errorController ? root.errorController.count : 0
    readonly property bool hasUnseenError: root.errorController !== null
        && root.errorController.latestUnseen
    readonly property string backendFailure: root.uiBridge.backendFailure || ""
    readonly property bool backendStopped: root.backendFailure.length > 0
    readonly property bool isDisconnected: !root.uiBridge.connected && !root.backendStopped
    readonly property bool showTrackSections: !root.isDisconnected && !root.backendStopped

    readonly property string channelText: (root.uiBridge.currentTrackChannelLayoutText || "").trim()
    readonly property string channelIconKey: (root.uiBridge.currentTrackChannelLayoutIconKey || "").trim()
    readonly property string channelIconPath: root.channelStatusIconSource(root.channelIconKey)
    readonly property string formatLabel: (root.uiBridge.currentTrackFormatLabel || "").trim()
    readonly property string bitDepthSampleRateText: FormatUtils.formatBitDepthSampleRateText(
        root.uiBridge.currentTrackBitDepth, root.uiBridge.currentTrackSampleRateHz)
    readonly property int bitrateKbps: {
        const v = Number(root.uiBridge.currentTrackCurrentBitrateKbps)
        return isFinite(v) && v > 0 ? Math.round(v) : 0
    }
    readonly property string playlistSummary: {
        const count = Math.max(0, Number(root.uiBridge.queueLength) || 0)
        return I18n.i18ncp("@info:status queue length and total duration",
            "%1 track (%2)", "%1 tracks (%2)",
            count, root.uiBridge.queueDurationText || "00:00")
    }

    // The bridge's playback state is a code, not text to show.
    function playbackStateText(state) {
        switch (state) {
        case "Playing":
            return I18n.i18nc("@info:status playback state", "Playing")
        case "Paused":
            return I18n.i18nc("@info:status playback state", "Paused")
        default:
            return I18n.i18nc("@info:status playback state", "Stopped")
        }
    }

    contentItem: RowLayout {
        spacing: 6

        Label {
            objectName: "statusBarBackendStoppedLabel"
            visible: root.backendStopped
            text: I18n.i18nc("@info:status", "Backend stopped")
            color: ColorUtils.mixColor(
                Kirigami.Theme.negativeTextColor,
                root.uiPalette.uiTextColor,
                root.themeIsDark ? 0.18 : 0.08)
            font.weight: Font.DemiBold
        }

        Label {
            visible: root.isDisconnected
            text: I18n.i18nc("@info:status", "Bridge disconnected")
            color: root.uiPalette.uiTextColor
        }

        Label {
            visible: root.showTrackSections
            objectName: "statusBarPlaybackStateLabel"
            text: root.playbackStateText(root.uiBridge.playbackState)
            color: root.uiPalette.uiTextColor
        }

        Label {
            visible: root.showTrackSections
            text: "|"
            color: root.uiPalette.uiMutedTextColor
        }
        Label {
            visible: root.showTrackSections
            text: I18n.i18nc("@info:status playback position of duration", "%1/%2",
                root.positionText, root.uiBridge.durationText || "00:00")
            color: root.uiPalette.uiTextColor
        }

        Label {
            visible: root.showTrackSections && root.formatLabel.length > 0
            text: "|"
            color: root.uiPalette.uiMutedTextColor
        }
        Label {
            visible: root.showTrackSections && root.formatLabel.length > 0
            text: root.formatLabel
            color: root.uiPalette.uiTextColor
        }

        Label {
            visible: root.showTrackSections && root.channelText.length > 0
            text: "|"
            color: root.uiPalette.uiMutedTextColor
        }
        RowLayout {
            visible: root.showTrackSections && root.channelText.length > 0
            spacing: root.channelIconPath.length > 0 ? 4 : 0

            Item {
                visible: root.channelIconPath.length > 0
                Layout.preferredWidth: visible ? 22 : 0
                Layout.preferredHeight: 20

                Kirigami.Icon {
                    anchors.fill: parent
                    source: root.channelIconPath
                    color: root.uiPalette.uiTextColor
                    isMask: true
                }
            }

            Label {
                text: root.channelText
                color: root.uiPalette.uiTextColor
            }
        }

        Label {
            visible: root.showTrackSections && root.bitDepthSampleRateText.length > 0
            text: "|"
            color: root.uiPalette.uiMutedTextColor
        }
        Label {
            visible: root.showTrackSections && root.bitDepthSampleRateText.length > 0
            text: root.bitDepthSampleRateText
            color: root.uiPalette.uiTextColor
        }

        Label {
            visible: root.showTrackSections && root.bitrateKbps > 0
            text: "|"
            color: root.uiPalette.uiMutedTextColor
        }
        Label {
            visible: root.showTrackSections && root.bitrateKbps > 0
            text: I18n.i18nc("@info:status bitrate", "%1 kbps", root.bitrateKbps)
            color: root.uiPalette.uiTextColor
        }

        Label {
            visible: root.showTrackSections
            text: "|"
            color: root.uiPalette.uiMutedTextColor
        }
        Label {
            visible: root.showTrackSections
            Layout.fillWidth: true
            objectName: "statusBarQueueSummaryLabel"
            text: root.playlistSummary
            elide: Text.ElideRight
            color: root.uiPalette.uiTextColor
        }

        Item {
            visible: !root.showTrackSections
            Layout.fillWidth: true
        }

        Components.ScrobbleIndicator {
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            Layout.preferredHeight: 20
            onSettingsRequested: root.lastFmSettingsRequested()
        }

        // Opens the recent errors. With none there is nothing to show.
        ToolButton {
            objectName: "statusBarErrorsButton"
            visible: root.errorCount > 0
            Layout.preferredHeight: 20
            padding: 0
            leftPadding: 6
            rightPadding: 6
            icon.name: "dialog-warning"
            icon.width: 16
            icon.height: 16
            text: I18n.i18ncp("@action:button", "%1 error", "%1 errors", root.errorCount)
            display: AbstractButton.TextBesideIcon
            palette.buttonText: root.hasUnseenError
                ? Kirigami.Theme.highlightedTextColor
                : ColorUtils.mixColor(
                    Kirigami.Theme.negativeTextColor,
                    root.uiPalette.uiTextColor,
                    root.themeIsDark ? 0.18 : 0.08)
            Accessible.name: I18n.i18ncp("@action:button",
                "%1 error: show recent errors", "%1 errors: show recent errors", root.errorCount)
            // The status bar's only word on an error it has not shown yet.
            Accessible.description: root.hasUnseenError
                ? ErrorUtils.describe(root.errorController.latest)
                : ""
            ToolTip.visible: hovered
            ToolTip.text: I18n.i18nc("@info:tooltip", "Show recent errors")
            onClicked: root.errorController.historyRequested()

            Rectangle {
                objectName: "statusBarNewErrorHighlight"
                anchors.fill: parent
                z: -2
                visible: root.hasUnseenError
                radius: Kirigami.Units.cornerRadius
                color: Kirigami.Theme.negativeTextColor
            }
        }
    }
}
