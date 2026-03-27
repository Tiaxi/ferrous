import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/FormatUtils.js" as FormatUtils

ToolBar {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var channelStatusIconSource
    required property bool themeIsDark
    property string transientError: ""

    implicitHeight: Math.max(contentItem.implicitHeight, 20) + topPadding + bottomPadding
    leftPadding: 14
    rightPadding: 10
    topPadding: 2
    bottomPadding: 2

    readonly property bool hasError: root.transientError.length > 0
    readonly property bool isDisconnected: !root.uiBridge.connected && !root.hasError
    readonly property bool showTrackSections: !root.hasError && !root.isDisconnected

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
        const noun = count === 1 ? "track" : "tracks"
        return count.toString() + " " + noun + " (" + (root.uiBridge.queueDurationText || "00:00") + ")"
    }

    contentItem: RowLayout {
        spacing: 6

        Label {
            visible: root.hasError
            text: "Error: " + root.transientError
            color: ColorUtils.mixColor(
                Kirigami.Theme.negativeTextColor,
                root.uiPalette.uiTextColor,
                root.themeIsDark ? 0.18 : 0.08)
            font.weight: Font.DemiBold
        }

        Label {
            visible: root.isDisconnected
            text: "Bridge disconnected"
            color: root.uiPalette.uiTextColor
        }

        Label {
            visible: root.showTrackSections
            text: root.uiBridge.playbackState || "Stopped"
            color: root.uiPalette.uiTextColor
        }

        Label {
            visible: root.showTrackSections
            text: "|"
            color: root.uiPalette.uiMutedTextColor
        }
        Label {
            visible: root.showTrackSections
            text: (root.uiBridge.positionText || "00:00") + "/" + (root.uiBridge.durationText || "00:00")
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

                Image {
                    anchors.fill: parent
                    source: root.channelIconPath
                    asynchronous: false
                    fillMode: Image.PreserveAspectFit
                    smooth: false
                    sourceSize.width: 44
                    sourceSize.height: 40
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
            text: root.bitrateKbps + " kbps"
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
            text: root.playlistSummary
            elide: Text.ElideRight
            color: root.uiPalette.uiTextColor
        }
    }
}
