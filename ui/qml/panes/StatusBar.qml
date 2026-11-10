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

// The status bar (#373): 24 px of chrome with 12 px text. On the left the
// playback state, repeat and shuffle, and the track's format; on the right
// the library scan, the Last.fm slot and the issues. Groups are 16 px
// apart, with middle dots only inside a group and no dividers.
ToolBar {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property bool themeIsDark
    // The app's ErrorController; the status bar shows how many issues it
    // keeps, and stands out until the user has seen the newest.
    property var errorController: null
    // Whether the recent issues are open: the issues button shows it.
    property bool issuesOpen: false

    // The Last.fm slot asked for the Last.fm preferences.
    signal lastFmSettingsRequested()

    // The narrow window's status bar: the format only, and the scan
    // without its folder and bar.
    readonly property bool narrow: root.width < 1200

    implicitHeight: 25
    leftPadding: 12
    rightPadding: 6
    topPadding: 1
    bottomPadding: 0

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
    // New until the user has seen them in the history.
    readonly property int unseenErrorCount: root.errorController
        ? Math.max(0, Number(root.errorController.unseenCount) || 0)
        : 0
    readonly property bool hasUnseenError: root.unseenErrorCount > 0
    readonly property string backendFailure: root.uiBridge.backendFailure || ""
    readonly property bool backendStopped: root.backendFailure.length > 0
    readonly property bool isDisconnected: !root.uiBridge.connected && !root.backendStopped
    readonly property bool showTrackSections: !root.isDisconnected && !root.backendStopped
    readonly property string playbackState: root.uiBridge.playbackState || "Stopped"

    readonly property string channelText: (root.uiBridge.currentTrackChannelLayoutText || "").trim()
    readonly property string formatLabel: (root.uiBridge.currentTrackFormatLabel || "").trim()
    readonly property string bitDepthSampleRateText: FormatUtils.formatBitDepthSampleRateText(
        root.uiBridge.currentTrackBitDepth, root.uiBridge.currentTrackSampleRateHz)
    readonly property int bitrateKbps: {
        const v = Number(root.uiBridge.currentTrackCurrentBitrateKbps)
        return isFinite(v) && v > 0 ? Math.round(v) : 0
    }
    // "FLAC · 16-bit / 44.1 kHz · Stereo · 557 kbps"; only "FLAC" when
    // narrow. Nothing while stopped.
    readonly property var formatParts: {
        if (root.playbackState === "Stopped") {
            return []
        }
        const parts = []
        if (root.formatLabel.length > 0) {
            parts.push(root.formatLabel)
        }
        if (root.narrow) {
            return parts
        }
        if (root.bitDepthSampleRateText.length > 0) {
            parts.push(root.bitDepthSampleRateText)
        }
        if (root.channelText.length > 0) {
            parts.push(root.channelText)
        }
        if (root.bitrateKbps > 0) {
            parts.push(I18n.i18nc("@info:status bitrate", "%1 kbps", root.bitrateKbps))
        }
        return parts
    }

    readonly property bool scanning: root.uiBridge.libraryScanInProgress === true
    readonly property int scanProcessed: Math.max(0, Number(root.uiBridge.libraryScanProcessed) || 0)
    // The files the scan will have processed when it ends: more than those
    // processed while work remains. A scan that cannot tell, as the first
    // scan of a folder, shows a busy bar and its count instead.
    readonly property int scanTotal: {
        const total = Number(root.uiBridge.libraryScanTotal)
        return isFinite(total) && total > 0 ? Math.max(root.scanProcessed, total) : -1
    }
    readonly property bool scanTotalKnown: root.scanTotal > 0
    // The folder being scanned, by name: known when the library has one.
    readonly property string scanFolderName: {
        const entries = root.uiBridge.libraryRootEntries || []
        if (entries.length !== 1) {
            return ""
        }
        const entry = entries[0]
        const name = String(entry.name || "").trim()
        return name.length > 0 ? name : FormatUtils.basenameFromPath(String(entry.path || ""))
    }
    readonly property string scanCountText: root.scanTotalKnown
        ? I18n.i18nc("@info:status files scanned of files to scan", "%1 / %2",
            root.formatCount(root.scanProcessed), root.formatCount(root.scanTotal))
        : root.scanProcessed > 0
            ? I18n.i18ncp("@info:status %2 is the files scanned so far", "%2 file", "%2 files",
                root.scanProcessed, root.formatCount(root.scanProcessed))
            : ""

    // A count with the locale's digit grouping: "1,204".
    function formatCount(count) {
        return Number(count).toLocaleString(Qt.locale(), "f", 0)
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

    // A 12 px label of the status bar.
    component StatusLabel: Label {
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
        color: root.uiPalette.textSecondary
        elide: Text.ElideRight
    }

    // A glyph of the status bar: a theme icon drawn in `color`.
    component StatusGlyph: Kirigami.Icon {
        property int size: 14
        implicitWidth: size
        implicitHeight: size
        color: root.uiPalette.textSecondary
        isMask: true
    }

    contentItem: RowLayout {
        spacing: 16

        StatusLabel {
            objectName: "statusBarBackendStoppedLabel"
            visible: root.backendStopped
            text: I18n.i18nc("@info:status", "Backend stopped")
            color: root.uiPalette.negative
            font.weight: Font.DemiBold
        }

        StatusLabel {
            visible: root.isDisconnected
            text: I18n.i18nc("@info:status", "Bridge disconnected")
            color: root.uiPalette.text
        }

        // The neutral playback state: the play, pause or stop glyph and the
        // word, which starts at the same place in every state.
        Row {
            objectName: "statusBarPlaybackState"
            visible: root.showTrackSections
            spacing: 6
            Layout.alignment: Qt.AlignVCenter

            StatusGlyph {
                objectName: "statusBarPlaybackGlyph"
                anchors.verticalCenter: parent.verticalCenter
                size: 11
                source: root.playbackState === "Playing" ? "media-playback-start"
                    : (root.playbackState === "Paused" ? "media-playback-pause" : "media-playback-stop")
            }

            StatusLabel {
                objectName: "statusBarPlaybackStateLabel"
                anchors.verticalCenter: parent.verticalCenter
                text: root.playbackStateText(root.playbackState)
                color: root.playbackState === "Stopped" ? root.uiPalette.textSecondary : root.uiPalette.text
            }
        }

        // Repeat, in words, and shuffle; nothing when both are off.
        Row {
            objectName: "statusBarRepeatShuffle"
            visible: root.showTrackSections && (root.uiBridge.repeatMode === 1
                || root.uiBridge.repeatMode === 2 || root.uiBridge.shuffleEnabled === true)
            spacing: 16
            Layout.alignment: Qt.AlignVCenter

            Row {
                visible: root.uiBridge.repeatMode === 1 || root.uiBridge.repeatMode === 2
                spacing: 6
                anchors.verticalCenter: parent.verticalCenter

                StatusGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    source: root.uiBridge.repeatMode === 1 ? "media-playlist-repeat-song" : "media-playlist-repeat"
                }

                StatusLabel {
                    objectName: "statusBarRepeatLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.uiBridge.repeatMode === 1
                        ? I18n.i18nc("@info:status", "Repeat One Track")
                        : I18n.i18nc("@info:status", "Repeat Whole Queue")
                }
            }

            StatusGlyph {
                objectName: "statusBarShuffleGlyph"
                anchors.verticalCenter: parent.verticalCenter
                visible: root.uiBridge.shuffleEnabled === true
                source: "media-playlist-shuffle"
                Accessible.role: Accessible.StaticText
                Accessible.name: I18n.i18nc("@info:status", "Shuffle On")

                HoverHandler {
                    id: shuffleHover
                }
                ToolTip.visible: shuffleHover.hovered
                ToolTip.delay: Kirigami.Units.toolTipDelay
                ToolTip.text: I18n.i18nc("@info:tooltip", "Shuffle On")
            }
        }

        // The format group: "FLAC · 16-bit / 44.1 kHz · Stereo · 557 kbps".
        // It fills the room left, and is cut at its end when there is too
        // little.
        Item {
            objectName: "statusBarFormat"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.fillHeight: true
            clip: formatRow.implicitWidth > width
            readonly property string text: root.formatParts.join(" · ")

            Row {
                id: formatRow
                anchors.verticalCenter: parent.verticalCenter
                visible: root.showTrackSections

                Repeater {
                    model: root.formatParts

                    delegate: Row {
                        required property int index
                        required property string modelData

                        StatusLabel {
                            visible: index > 0
                            text: "·"
                            color: root.uiPalette.textTertiary
                            leftPadding: 6
                            rightPadding: 6
                            Accessible.ignored: true
                        }

                        StatusLabel {
                            text: modelData
                        }
                    }
                }
            }
        }

        // The library scan: "Scanning “Music” 412 / 1,204" and a bar that
        // moves with the scan's batches; narrow: "Scanning 412 / 1,204".
        // Without a total: "Scanning “Music” 412 files" and a busy bar.
        Row {
            id: scanGroup
            objectName: "statusBarScan"
            visible: root.scanning && root.showTrackSections
            spacing: Kirigami.Units.largeSpacing
            Layout.alignment: Qt.AlignVCenter
            readonly property real fraction: root.scanTotalKnown
                ? Math.min(1, root.scanProcessed / root.scanTotal)
                : 0
            Accessible.role: Accessible.StaticText
            Accessible.name: scanToolTipText.text

            StatusLabel {
                objectName: "statusBarScanLabel"
                anchors.verticalCenter: parent.verticalCenter
                text: !root.narrow && root.scanFolderName.length > 0
                    ? I18n.i18nc("@info:status %1 is a library folder", "Scanning “%1”", root.scanFolderName)
                    : I18n.i18nc("@info:status", "Scanning")
            }

            StatusLabel {
                objectName: "statusBarScanCount"
                anchors.verticalCenter: parent.verticalCenter
                visible: text.length > 0
                text: root.scanCountText
            }

            Rectangle {
                objectName: "statusBarScanBar"
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.narrow
                width: 72
                height: 4
                radius: 2
                color: ColorUtils.withAlpha(root.uiPalette.text, 0.15)

                Rectangle {
                    objectName: "statusBarScanBarFill"
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    visible: root.scanTotalKnown
                    // Whole pixels, rounded down: the bar moves with the
                    // batches, and is full only once the scan is.
                    width: Math.floor(parent.width * scanGroup.fraction)
                    radius: 2
                    color: root.uiPalette.textSecondary
                }

                // Busy: a third of the bar goes back and forth; it stays
                // at the start when the desktop turns animations off.
                Rectangle {
                    id: scanBusy
                    objectName: "statusBarScanBarBusy"
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    visible: !root.scanTotalKnown
                    width: Math.round(parent.width / 3)
                    radius: 2
                    color: root.uiPalette.textSecondary

                    SequentialAnimation on x {
                        objectName: "statusBarScanBarBusyAnimation"
                        running: scanBusy.visible && Kirigami.Units.longDuration > 0
                        loops: Animation.Infinite
                        NumberAnimation {
                            from: 0
                            to: scanBusy.parent.width - scanBusy.width
                            duration: Kirigami.Units.veryLongDuration * 3
                            easing.type: Easing.InOutQuad
                        }
                        NumberAnimation {
                            from: scanBusy.parent.width - scanBusy.width
                            to: 0
                            duration: Kirigami.Units.veryLongDuration * 3
                            easing.type: Easing.InOutQuad
                        }
                    }
                }
            }

            HoverHandler {
                id: scanHover
            }
            ToolTip.visible: scanHover.hovered
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: scanToolTipText.text

            QtObject {
                id: scanToolTipText
                readonly property string text: {
                    const files = root.scanTotalKnown
                        ? I18n.i18ncp("@info:tooltip %2 and %3 are file counts",
                            "%2 of %3 file", "%2 of %3 files", root.scanTotal,
                            root.formatCount(root.scanProcessed), root.formatCount(root.scanTotal))
                        : root.scanProcessed > 0
                            ? I18n.i18ncp("@info:tooltip %2 is the files scanned so far",
                                "%2 file so far", "%2 files so far", root.scanProcessed,
                                root.formatCount(root.scanProcessed))
                            : ""
                    const folder = root.scanFolderName.length > 0
                        ? I18n.i18nc("@info:tooltip %1 is a library folder", "Scanning “%1”",
                            root.scanFolderName)
                        : I18n.i18nc("@info:tooltip", "Scanning the library")
                    return files.length > 0
                        ? I18n.i18nc("@info:tooltip scan, then its files", "%1: %2", folder, files)
                        : folder
                }
            }
        }

        Components.ScrobbleIndicator {
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            Layout.alignment: Qt.AlignVCenter
            onSettingsRequested: root.lastFmSettingsRequested()
        }

        // The issues: "2 new issues" with a negative warning glyph until
        // the user has seen them, then "3 issues" in the secondary colour.
        // It opens Recent Issues; with none there is nothing to show.
        ToolButton {
            id: issuesButton
            objectName: "statusBarErrorsButton"
            visible: root.errorCount > 0
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: leftPadding + issuesRow.implicitWidth + rightPadding
            implicitHeight: 22
            padding: 0
            leftPadding: 6
            rightPadding: 6
            flat: true
            focusPolicy: Qt.TabFocus
            text: root.hasUnseenError
                ? I18n.i18ncp("@action:button", "%1 new issue", "%1 new issues", root.unseenErrorCount)
                : I18n.i18ncp("@action:button", "%1 issue", "%1 issues", root.errorCount)
            readonly property color glyphColor: root.hasUnseenError
                ? root.uiPalette.negative
                : root.uiPalette.textSecondary
            readonly property color labelColor: root.hasUnseenError
                ? root.uiPalette.text
                : root.uiPalette.textSecondary
            Accessible.name: I18n.i18nc("@action:button %1 is the issue count", "%1: show recent issues",
                issuesButton.text)
            // The status bar's only word on an issue it has not shown yet.
            Accessible.description: root.hasUnseenError
                ? ErrorUtils.describe(root.errorController.latest)
                : ""
            // Keyboard focus shows the same tooltip as hover.
            ToolTip.visible: (hovered || visualFocus) && !root.issuesOpen
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: I18n.i18nc("@info:tooltip", "Show Recent Issues")
            onClicked: root.errorController.historyRequested()

            background: Rectangle {
                objectName: "statusBarErrorsBackground"
                radius: 3
                // A text-10% tint while the issues are open or pressed.
                color: root.issuesOpen || issuesButton.down
                    ? ColorUtils.withAlpha(root.uiPalette.text, 0.10)
                    : (issuesButton.hovered ? root.uiPalette.hover : "transparent")
                border.width: issuesButton.visualFocus ? 2 : 0
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
                implicitWidth: issuesRow.implicitWidth
                implicitHeight: issuesRow.implicitHeight

                Row {
                    id: issuesRow
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    StatusGlyph {
                        objectName: "statusBarErrorsGlyph"
                        anchors.verticalCenter: parent.verticalCenter
                        source: Qt.resolvedUrl("../assets/status-warning.svg")
                        color: issuesButton.glyphColor
                    }

                    StatusLabel {
                        objectName: "statusBarErrorsLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        text: issuesButton.text
                        color: issuesButton.labelColor
                        Accessible.ignored: true
                    }
                }
            }
        }
    }
}
