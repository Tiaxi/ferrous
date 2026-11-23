// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/ErrorUtils.js" as ErrorUtils
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils
import "../components" as Components

// The status bar (#373): 24 px of chrome with 12 px text, taller with a
// larger font. On the left the playback state, repeat and shuffle, and the
// track's format; on the right the library scan, the Last.fm slot and the
// issues. Groups are 16 px apart, with middle dots only inside a group and
// no dividers.
// An error has one treatment here (it had three: red semibold words for a
// stopped engine, plain words for one not running, and a glyph with words
// for the issues and a failed scrobble): its words in the text colour
// beside a 16 px glyph in the negative colour. Severity has one shape and
// one colour wherever an issue shows: an error's ring with a cross in the
// negative colour, a warning's triangle in the secondary one.
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

    // The narrow window's status bar: the scan without its folder and bar.
    // The format group goes by the room it has instead.
    readonly property bool narrow: root.width < 1200

    // The bar under its 1 px seam: 24 px, or the small text's line and 4 px
    // above and below it when that is more, or the Last.fm glyph clear of
    // the slot's focus ring, with 1 px above and below, at a font where
    // that is more.
    readonly property real barHeight: Math.max(24, Math.round(smallMetrics.height) + 8,
        scrobbleIndicator.minimumBlockHeight + 2)

    implicitHeight: root.topPadding + root.barHeight
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
    // Whether an error, rather than warnings alone, is among the new
    // issues, or among them all once they are seen.
    readonly property bool unseenIncludesError: root.errorController
        ? (Number(root.errorController.unseenErrorCount) || 0) > 0
        : false
    readonly property bool issuesIncludeError: root.hasUnseenError
        ? root.unseenIncludesError
        : (root.errorController ? (Number(root.errorController.errorEntryCount) || 0) > 0 : false)
    readonly property url errorGlyph: Qt.resolvedUrl("../assets/status-error.svg")
    readonly property url warningGlyph: Qt.resolvedUrl("../assets/status-warning.svg")
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
    // "FLAC · 16-bit / 44.1 kHz · Stereo · 557 kbps", of which the format
    // group shows as many as fit. Nothing while stopped.
    readonly property var formatParts: {
        if (root.playbackState === "Stopped") {
            return []
        }
        const parts = []
        if (root.formatLabel.length > 0) {
            parts.push(root.formatLabel)
        }
        if (root.bitDepthSampleRateText.length > 0) {
            parts.push(root.bitDepthSampleRateText)
        }
        if (root.channelText.length > 0) {
            parts.push(root.channelText)
        }
        if (root.bitrateKbps > 0) {
            parts.push(root.bitrateText(root.bitrateKbps))
        }
        return parts
    }

    function bitrateText(kbps) {
        return I18n.i18nc("@info:status bitrate", "%1 kbps", kbps)
    }
    // The room of a bitrate of four digits, in tabular figures.
    readonly property real bitrateRoom: Math.ceil(bitrateMetrics.advanceWidth)

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
    // The share of the files to scan that are scanned, in whole percent,
    // rounded down: 100 only once the scan is done.
    readonly property int scanPercent: root.scanTotalKnown
        ? Math.floor(Math.min(1, root.scanProcessed / root.scanTotal) * 100)
        : 0
    // The scan's progress beside its label: its percent once it has a
    // total, and the files scanned so far until then. The files to scan
    // and those scanned are in the tooltip.
    readonly property string scanCountText: root.scanTotalKnown
        ? root.percentText(root.scanPercent)
        : root.scanProcessed > 0
            ? I18n.i18ncp("@info:status %2 is the files scanned so far", "%2 file", "%2 files",
                root.scanProcessed, root.formatCount(root.scanProcessed))
            : ""

    // A percent as the status bar shows it: "42%".
    function percentText(percent) {
        return I18n.i18nc("@info:status percent of the library scanned", "%1%", percent)
    }

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

    FontMetrics {
        id: smallMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
    }

    TextMetrics {
        id: bitrateMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
        // As a number, which is written as a bitrate is.
        text: root.bitrateText(8888)
    }

    // A 12 px label of the status bar.
    component StatusLabel: Label {
        textFormat: Text.PlainText
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
        color: root.uiPalette.textSecondary
        elide: Text.ElideRight
    }

    // An error's glyph, and Repeat's and Shuffle's: 16 px, as the Last.fm
    // slot's, the size Breeze draws the modes for (at 14 px their lines
    // were soft).
    readonly property int smallGlyphSize: Kirigami.Units.iconSizes.small
    readonly property int errorGlyphSize: root.smallGlyphSize

    // A glyph of the status bar: a theme icon drawn in `color`, `size` at
    // the default font and larger with a larger one, at that size rather
    // than rounded down to a theme icon size.

    // How far above the status bar's seam every tooltip of its items ends:
    // one line for all of them, as Recent Issues stands off it (the engine's
    // tooltip floated over it, the others sat on it).
    readonly property int toolTipClearance: Kirigami.Units.smallSpacing + 2

    // A tooltip of one of the bar's items, `owner`, a child of its row: its
    // text as it is, never as markup (a folder's name or Last.fm's words
    // are in some), centred over the item and clear of the bar.
    component StatusToolTip: Components.PlainToolTip {
        required property Item owner
        y: -(root.contentItem.y + owner.y) - height - root.toolTipClearance
        delay: Kirigami.Units.toolTipDelay
    }

    component StatusGlyph: Kirigami.Icon {
        property int size: 14
        implicitWidth: root.uiPalette.scaledSize(size)
        implicitHeight: root.uiPalette.scaledSize(size)
        roundToIconSize: false
        color: root.uiPalette.textSecondary
        isMask: true
    }

    contentItem: RowLayout {
        spacing: 16

        // The playback engine, the app's backend, has stopped with a
        // failure, or has not started or has stopped without one to report:
        // an error, in the user's words, with why and what to do in the
        // tooltip. The glyph and the words are one: the pointer over either
        // shows the tooltip (over the glyph it showed none), and keyboard
        // focus shows it too, in the status bar's focus ring, 6 px outside
        // them as the issues button's is.
        Control {
            id: engineError
            objectName: "statusBarEngineError"
            visible: root.backendStopped || root.isDisconnected
            Layout.alignment: Qt.AlignVCenter
            implicitHeight: root.barHeight - 2
            // None of the style's padding: the glyph where the playback
            // state's is, and the ring outside them.
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            leftInset: -6
            rightInset: -6
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            readonly property string text: root.backendStopped
                ? I18n.i18nc("@info:status", "Playback engine stopped")
                : I18n.i18nc("@info:status", "Playback engine not running")
            // Why it stopped, in the backend's words, or what to do.
            readonly property string toolTipText: root.backendStopped
                ? root.backendFailure
                : I18n.i18nc("@info:tooltip",
                    "Ferrous can't play music until its playback engine runs. Restart Ferrous to try again.")
            Accessible.role: Accessible.StaticText
            Accessible.name: engineError.text
            Accessible.description: engineError.toolTipText

            StatusToolTip {
                objectName: "statusBarEngineErrorToolTip"
                owner: engineError
                message: engineError.toolTipText
                visible: (engineError.hovered || engineError.visualFocus) && engineError.toolTipText.length > 0
            }

            background: Rectangle {
                objectName: "statusBarEngineErrorBackground"
                radius: 3
                color: "transparent"
                border.width: engineError.visualFocus ? 2 : 0
                border.color: root.uiPalette.focusRing
            }

            contentItem: Item {
                implicitWidth: engineErrorRow.implicitWidth
                implicitHeight: engineErrorRow.implicitHeight

                Row {
                    id: engineErrorRow
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    StatusGlyph {
                        objectName: "statusBarEngineErrorGlyph"
                        anchors.verticalCenter: parent.verticalCenter
                        size: root.errorGlyphSize
                        source: root.errorGlyph
                        color: root.uiPalette.negative
                    }

                    StatusLabel {
                        objectName: "statusBarBackendStoppedLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.backendStopped
                        text: I18n.i18nc("@info:status", "Playback engine stopped")
                        color: root.uiPalette.text
                        Accessible.ignored: true
                    }

                    StatusLabel {
                        objectName: "statusBarDisconnectedLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.isDisconnected
                        text: I18n.i18nc("@info:status", "Playback engine not running")
                        color: root.uiPalette.text
                        Accessible.ignored: true
                    }
                }
            }
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

        // The format, then repeat and shuffle, and the bar's free room after
        // them: the format keeps one x as the modes toggle (after them, it
        // jumped some 210 px). Not a layout: the format is as wide as the
        // parts of it that fit the room the modes leave, so that they
        // follow what it shows, and a layout would feed that back into the
        // room.
        Item {
            id: leadingGroups
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: formatGroup.wholeWidth + leadingGroups.modesRoom
            // The modes whole, at the format's x once none of it fits: no
            // gap before them then.
            Layout.minimumWidth: modesRow.visible ? Math.ceil(modesRow.width) : 0
            readonly property real modesGap: root.formatParts.length > 0 ? 16 : 0
            readonly property real modesRoom: modesRow.visible
                ? Math.ceil(modesRow.width) + leadingGroups.modesGap : 0

            // The format group: "FLAC · 16-bit / 44.1 kHz · Stereo · 557 kbps".
            // It shows as many of its parts as fit whole in the room the
            // modes leave, dropping them from the end (it went by the
            // window's width, and showed "FLAC" alone with most of the bar
            // empty), and is as wide as those. A bitrate has the room of
            // four digits, so that one that changes doesn't move the modes
            // after it.
            Item {
                id: formatGroup
                objectName: "statusBarFormat"
                height: parent.height
                width: formatGroup.shownCount < root.formatParts.length
                    ? (formatGroup.shownCount > 0 ? formatGroup.partEnds[formatGroup.shownCount - 1] : 0)
                    : Math.min(formatGroup.wholeWidth, formatGroup.room)
                clip: formatRow.implicitWidth > width
                readonly property real room: Math.max(0, Math.floor(leadingGroups.width - leadingGroups.modesRoom))
                // Where each part ends, after the first with the separator
                // before it, and the bitrate with its room.
                readonly property var partEnds: {
                    const parts = root.formatParts
                    const dot = formatGroup.widthOf(FormatUtils.fieldSeparator)
                    const bitrateAt = root.bitrateKbps > 0 ? parts.length - 1 : -1
                    let end = 0
                    return parts.map((part, i) => {
                        const width = formatGroup.widthOf(part)
                        end += (i === bitrateAt ? Math.max(root.bitrateRoom, width) : width) + (i > 0 ? dot : 0)
                        return end
                    })
                }
                // All of it, with a bitrate's room also while none is known.
                readonly property real wholeWidth: formatGroup.partEnds.length === 0 ? 0
                    : formatGroup.partEnds[formatGroup.partEnds.length - 1] + (root.bitrateKbps > 0 ? 0
                        : formatGroup.widthOf(FormatUtils.fieldSeparator) + root.bitrateRoom)
                // In whole pixels of the font as it is: advanceWidth() alone
                // tells no binding that the font it measures in has changed,
                // and the first widths are in the font before the label's.
                function widthOf(text) {
                    return formatMetrics.font ? Math.ceil(formatMetrics.advanceWidth(text)) : 0
                }
                // How many of the parts fit.
                readonly property int shownCount: {
                    const ends = formatGroup.partEnds
                    for (let i = 0; i < ends.length; ++i) {
                        if (ends[i] > formatGroup.room) {
                            return i
                        }
                    }
                    return ends.length
                }
                readonly property var shownParts: root.formatParts.slice(0, formatGroup.shownCount)
                readonly property string text: formatGroup.shownParts.join(FormatUtils.fieldSeparator)

                // The status bar's 12 px label font, with its tabular figures.
                FontMetrics {
                    id: formatMetrics
                    font: formatProbe.font
                }
                StatusLabel {
                    id: formatProbe
                    visible: false
                    Accessible.ignored: true
                }

                Row {
                    id: formatRow
                    objectName: "statusBarFormatRow"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.showTrackSections

                    Repeater {
                        model: formatGroup.shownParts

                        delegate: Row {
                            required property int index
                            required property string modelData

                            // The headers' separator, a middle dot between
                            // two spaces, at their spacing (a dot
                            // with 6 px either side stood wider than theirs).
                            StatusLabel {
                                objectName: "statusBarFormatSeparator"
                                visible: index > 0
                                text: FormatUtils.fieldSeparator
                                color: root.uiPalette.textTertiary
                                Accessible.ignored: true
                            }

                            StatusLabel {
                                textFormat: Text.PlainText
                                text: modelData
                            }
                        }
                    }
                }
            }

            // Repeat and shuffle, each a glyph and its menu item's words, 16 px
            // after the format; nothing when both are off.
            Row {
                id: modesRow
                objectName: "statusBarRepeatShuffle"
                visible: root.showTrackSections && (root.uiBridge.repeatMode === 1
                    || root.uiBridge.repeatMode === 2 || root.uiBridge.shuffleEnabled === true)
                spacing: 16
                x: formatGroup.width > 0 ? formatGroup.width + leadingGroups.modesGap : 0
                y: Math.round((parent.height - height) / 2)

                Row {
                    visible: root.uiBridge.repeatMode === 1 || root.uiBridge.repeatMode === 2
                    spacing: 6
                    anchors.verticalCenter: parent.verticalCenter

                    StatusGlyph {
                        objectName: "statusBarRepeatGlyph"
                        anchors.verticalCenter: parent.verticalCenter
                        size: root.smallGlyphSize
                        source: root.uiBridge.repeatMode === 1 ? "media-playlist-repeat-song" : "media-playlist-repeat"
                    }

                    StatusLabel {
                        objectName: "statusBarRepeatLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        // Whole pixels, for Shuffle's glyph after it.
                        width: Math.ceil(implicitWidth)
                        text: root.uiBridge.repeatMode === 1
                            ? I18n.i18nc("@info:status", "Repeat One Track")
                            : I18n.i18nc("@info:status", "Repeat Whole Queue")
                    }
                }

                Row {
                    objectName: "statusBarShuffle"
                    visible: root.uiBridge.shuffleEnabled === true
                    spacing: 6
                    anchors.verticalCenter: parent.verticalCenter

                    StatusGlyph {
                        objectName: "statusBarShuffleGlyph"
                        anchors.verticalCenter: parent.verticalCenter
                        size: root.smallGlyphSize
                        source: "media-playlist-shuffle"
                    }

                    StatusLabel {
                        objectName: "statusBarShuffleLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        text: I18n.i18nc("@info:status", "Shuffle")
                    }
                }
            }
        }

        // The library scan: "Scanning “Music”…", a bar that moves with the
        // scan's batches, and "34%"; narrow: "Scanning… 34%". Its words say
        // what the bar is (a bare bar read as a twin of the
        // Last.fm slot's). Without a total: "Scanning “Music”… 412 files",
        // the count being the progress; the bar shows once there is a total
        // to fill it against. The parts are one unit, 6 px apart, as a glyph
        // and its words are (the percent, right-aligned in the room of
        // "100%" after the words, stood some 16 px off them). The percent
        // keeps the width of "100%" after it, in tabular figures, so nothing
        // in the status bar moves as it counts.
        // Nothing in it moves on its own (#373: no indeterminate
        // animation), so it is still with the desktop's animations off.
        Row {
            id: scanGroup
            objectName: "statusBarScan"
            visible: root.scanning && root.showTrackSections
            spacing: Kirigami.Units.smallSpacing + 2
            Layout.alignment: Qt.AlignVCenter
            readonly property real fraction: root.scanTotalKnown
                ? Math.min(1, root.scanProcessed / root.scanTotal)
                : 0
            Accessible.role: Accessible.StaticText
            Accessible.name: scanToolTipText.text

            StatusLabel {
                objectName: "statusBarScanLabel"
                anchors.verticalCenter: parent.verticalCenter
                text: root.narrow
                    ? I18n.i18nc("@info:status library scan in progress", "Scanning…")
                    : root.scanFolderName.length > 0
                        ? I18n.i18nc("@info:status %1 is a library folder", "Scanning “%1”…",
                            root.scanFolderName)
                        : I18n.i18nc("@info:status", "Scanning library…")
            }

            Rectangle {
                id: scanBar
                objectName: "statusBarScanBar"
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.narrow && root.scanTotalKnown
                // 72 by 6 px at the default font, and larger with a larger
                // one, as the Last.fm bar is: as long as the glyphs grow,
                // and as thick as that bar grows with its glyph. Its frame
                // is the Last.fm trough's, and the fill inside it 4 px.
                readonly property int frame: scrobbleIndicator.troughFrame
                width: root.uiPalette.scaledSize(72)
                height: Math.max(4, Math.round(root.uiPalette.scaledSize(root.smallGlyphSize) / 5))
                    + 2 * frame
                radius: height / 2
                // The status bar's progress trough and fill, the Last.fm
                // bar's: a frame at 3:1 on the status bar around the view,
                // and in it the fill, the secondary text, never louder than
                // the words beside it, so that the share scanned reads
                // against the rest (a solid trough midway between the fill
                // and the status bar was 2.7:1 / 2.5:1 to each, and in a
                // light theme it washed out into the status bar).
                color: root.uiPalette.statusBarTrough
                border.width: frame
                border.color: root.uiPalette.statusBarTroughFrame

                Rectangle {
                    objectName: "statusBarScanBarFill"
                    x: scanBar.frame
                    y: scanBar.frame
                    height: scanBar.height - 2 * scanBar.frame
                    // Whole pixels of the inside, rounded down: the bar
                    // moves with the batches, and is full only once the
                    // scan is.
                    width: Math.floor((scanBar.width - 2 * scanBar.frame) * scanGroup.fraction)
                    radius: height / 2
                    color: root.uiPalette.statusBarFill
                }
            }

            StatusLabel {
                id: scanCount
                objectName: "statusBarScanCount"
                anchors.verticalCenter: parent.verticalCenter
                visible: text.length > 0
                text: root.scanCountText
                // A percent keeps the room of the widest after it, the bar
                // and the words before it staying where they are.
                width: root.scanTotalKnown
                    ? Math.max(Math.ceil(implicitWidth), Math.ceil(fullPercentMetrics.advanceWidth))
                    : implicitWidth
                horizontalAlignment: Text.AlignLeft

                TextMetrics {
                    id: fullPercentMetrics
                    font: scanCount.font
                    text: root.percentText(100)
                }
            }

            HoverHandler {
                id: scanHover
            }
            StatusToolTip {
                objectName: "statusBarScanToolTip"
                owner: scanGroup
                message: scanToolTipText.text
                visible: scanHover.hovered
            }

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
            id: scrobbleIndicator
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            // Hidden with the other track sections while the engine is
            // stopped or not running.
            engineRunning: root.showTrackSections
            // The issues button's height, so its glyph is centred on the
            // status bar.
            blockHeight: root.barHeight - 2
            roomToBarTop: root.contentItem.y + y
            toolTipClearance: root.toolTipClearance
            Layout.alignment: Qt.AlignVCenter
            onSettingsRequested: root.lastFmSettingsRequested()
        }

        // The issues: "2 new issues" in the text colour until the user has
        // seen them, then "3 issues" in the secondary colour. The 16 px
        // glyph is the shape of the most severe of them, the new ones while
        // there are any: an error's ring with a cross, in the negative
        // colour while an error is new, otherwise the warning's triangle;
        // in the secondary colour once seen, and for warnings alone, which
        // the word "new" marks (every new issue was red, so a scan's
        // notice raised the alarm of a stopped engine).
        // It opens Recent Issues; with none there is nothing to show.
        ToolButton {
            id: issuesButton
            objectName: "statusBarErrorsButton"
            visible: root.errorCount > 0
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: leftPadding + issuesRow.implicitWidth + rightPadding
            implicitHeight: root.barHeight - 2
            padding: 0
            leftPadding: 6
            rightPadding: 6
            flat: true
            focusPolicy: Qt.TabFocus
            text: root.hasUnseenError
                ? I18n.i18ncp("@action:button", "%1 new issue", "%1 new issues", root.unseenErrorCount)
                : I18n.i18ncp("@action:button", "%1 issue", "%1 issues", root.errorCount)
            readonly property color glyphColor: root.hasUnseenError && root.unseenIncludesError
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
            StatusToolTip {
                objectName: "statusBarErrorsToolTip"
                owner: issuesButton
                message: I18n.i18nc("@info:tooltip", "Show Recent Issues")
                visible: (issuesButton.hovered || issuesButton.visualFocus) && !root.issuesOpen
            }
            onClicked: root.errorController.historyRequested()
            // Plain Space presses the focused button, not the window's
            // Play/Pause shortcut.
            Keys.onShortcutOverride: function(event) {
                event.accepted = KeyUtils.isPlainSpace(event)
            }

            background: Rectangle {
                objectName: "statusBarErrorsBackground"
                radius: 3
                // The pressed tint while pressed, and a text-10% one while
                // the issues are open.
                color: issuesButton.down ? root.uiPalette.pressed
                    : root.issuesOpen ? ColorUtils.withAlpha(root.uiPalette.text, 0.10)
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
                        size: root.errorGlyphSize
                        source: root.issuesIncludeError ? root.errorGlyph : root.warningGlyph
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
