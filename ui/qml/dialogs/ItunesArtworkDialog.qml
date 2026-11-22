// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/PathUtils.js" as PathUtils
import "../components" as Components
import "." as Dialogs

// Replace from iTunes, in a window of its own: KWin draws its title bar,
// and it moves and resizes beyond the window it is for, the main window or
// the album art viewer over the whole screen, which it blocks while it
// shows.
Components.DialogWindow {
    id: root

    required property var uiBridge
    // Open the viewer's preview of a candidate row, compared with the current
    // art, or of the current art; each returns whether the preview opened.
    required property var openAlbumArtViewerForSuggestion
    required property var openAlbumArtViewerForCurrentArt
    // Ends the viewer's preview; with true, after Apply, and the applied
    // image when playback has moved on from the album it went to.
    required property var endAlbumArtPreview
    // Where the viewer is, which a question asked from its preview shows
    // over: its window over the whole screen, or the main window's overlay.
    property var previewHost: null

    // Previewing is a step of the dialog: it hides, keeping its results,
    // their scroll position and focus, while the album art viewer shows
    // the candidate, or the current art for -1, and shows again where it was
    // when the viewer goes back to the results.
    property bool previewShown: false
    property int previewIndex: -1
    // The previewed candidate is ready to apply: downloaded, not still
    // loading or failed, and its image decodes. That holds for the
    // candidate, whichever image the viewer shows to compare.
    readonly property bool previewCandidateReady: root.previewShown && root.previewIndex >= 0
        && root.suggestionRowApplicable(root.previewRow)
        && previewCandidateProbe.status === Image.Ready
    property var previewRow: ({})
    property int pendingPreviewIndex: -1
    property int pendingApplyIndex: -1
    property string currentArtworkSource: ""
    property var currentArtworkInfo: ({})
    property real _savedContentY: 0
    property int _savedResultCount: 0

    title: I18n.i18nc("@title:window", "Replace from iTunes")
    // The content's inset from the window's edges.
    readonly property real contentPadding: Kirigami.Units.gridUnit
    // 920 px wide with the default font, and as much wider as a larger font
    // makes the text, up to 1280 px, within the screen: wider left a gulf
    // between the titles and their actions (as at 1964 px at 24 pt); as
    // tall as the current art and five results, which the list shows whole
    // (a fixed 680 px left a framed empty band under them).
    // The user may resize it.
    defaultWidth: Math.min(root.uiPalette.scaledSize(920), 1280)
    defaultHeight: Math.ceil(2 * root.contentPadding + root.rowHeight + root.headingHeight
        + root.shownResultRows * root.rowHeight + 2 + itunesArtworkFooter.implicitHeight)
    // The current art's row whole, and a result under it.
    minimumWidth: Math.ceil(Math.max(Kirigami.Units.gridUnit * 24, itunesArtworkFooter.implicitWidth))
    minimumHeight: Math.ceil(2 * root.contentPadding + 2 * currentArtworkRow.Layout.preferredHeight
        + Kirigami.Units.largeSpacing + itunesArtworkFooter.implicitHeight)

    footer: Components.DialogButtons {
        id: itunesArtworkFooter
        primary: itunesArtworkCloseButton

        Button {
            id: itunesArtworkCloseButton
            objectName: "itunesArtworkCloseButton"
            text: I18n.i18nc("@action:button", "Close")
            icon.name: "window-close"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    function clearPendingActionState() {
        root.pendingPreviewIndex = -1
        root.pendingApplyIndex = -1
    }

    // The art of the track the suggestions are for. Once playback has moved
    // on to another track, the art that track had when they were searched.
    function targetArtworkSource() {
        const target = root.uiBridge.itunesArtworkTarget || ({})
        const targetPath = target.trackPath || ""
        if (targetPath.length > 0 && targetPath !== (root.uiBridge.currentTrackPath || "")) {
            return target.coverPath || ""
        }
        return root.uiBridge.currentTrackCoverPath || ""
    }

    function refreshCurrentArtworkInfo() {
        root.currentArtworkSource = root.targetArtworkSource()
        const infoSource = PathUtils.pathFromAnyUrl(root.currentArtworkSource)
        if (infoSource.length <= 0) {
            root.currentArtworkInfo = ({})
            return
        }
        root.uiBridge.requestImageFileDetails(infoSource)
        root.currentArtworkInfo = root.uiBridge.cachedImageFileDetails(infoSource) || ({})
    }

    function suggestionRowAt(index) {
        return root.uiBridge.itunesArtworkResultAt(index) || ({})
    }

    function suggestionRowReady(row) {
        return ((row && (row.normalizedPath || "")) || "").length > 0
    }

    function suggestionRowApplicable(row) {
        return root.suggestionRowReady(row)
            && !((row && (row.assetLoading || false)) || false)
            && ((row && (row.assetError || "")) || "").length === 0
    }

    // Shows the candidate at `index`, or the current art for -1, in the
    // album art viewer, and hides the dialog once the viewer shows it. The
    // current art is the art of the album the suggestions are for.
    function showPreview(index) {
        const row = index >= 0 ? root.suggestionRowAt(index) : ({})
        if (index >= 0 ? !root.suggestionRowReady(row) : root.currentArtworkSource.length === 0) {
            return
        }
        const opened = index >= 0
            ? root.openAlbumArtViewerForSuggestion(row, root.currentArtworkSource)
            : root.openAlbumArtViewerForCurrentArt(root.currentArtworkSource)
        if (!opened) {
            return
        }
        root.clearPendingActionState()
        root._savedContentY = itunesArtworkResultsView.contentY
        root._savedResultCount = itunesArtworkResultsView.count
        root.previewIndex = index
        root.previewRow = row
        root.previewShown = true
        root.reject()
    }

    // The viewer went back to the results: the dialog shows again where it
    // was, with the focus on the Preview button that was pressed.
    function showAgainAfterPreview() {
        if (!root.previewShown) {
            return
        }
        root.previewShown = false
        root.open()
        if (itunesArtworkResultsView.count === root._savedResultCount) {
            itunesArtworkResultsView.contentY = root._savedContentY
        }
        const button = root.previewIndex >= 0
            ? ((itunesArtworkResultsView.itemAtIndex(root.previewIndex) || {}).previewButton || null)
            : currentArtworkPreviewButton
        if (button) {
            button.forceActiveFocus(Qt.OtherFocusReason)
        }
    }

    // Apply in the viewer's preview.
    function applyPreviewedSuggestion() {
        if (!root.previewCandidateReady) {
            return
        }
        root.applySuggestion(root.previewIndex)
    }

    // After Apply of the suggestion at `index`: the dialog closes, and a
    // viewer showing its preview goes on to show the applied cover: the
    // playing album's art, which becomes it, or, once playback has moved on
    // to another album, the applied image itself.
    function finishAfterApply(index) {
        if (!root.previewShown) {
            root.accept()
            return
        }
        const targetPath = (root.uiBridge.itunesArtworkTarget || ({})).trackPath || ""
        const movedOn = targetPath.length > 0 && targetPath !== (root.uiBridge.currentTrackPath || "")
        const appliedRow = root.suggestionRowAt(index)
        root.previewShown = false
        root.endAlbumArtPreview(true, movedOn ? (appliedRow.normalizedUrl || "") : "")
        root.clearAfterClose()
    }

    function clearAfterClose() {
        root.clearPendingActionState()
        // Its suggestion goes with the suggestions cleared here.
        replaceCoverConfirmDialog.dismiss()
        root.uiBridge.clearItunesArtworkSuggestions()
        root.previewIndex = -1
        root.previewRow = ({})
    }

    function requestSuggestionPreview(index) {
        const row = root.suggestionRowAt(index)
        if (root.suggestionRowReady(row)) {
            root.showPreview(index)
            return
        }
        root.pendingApplyIndex = -1
        root.pendingPreviewIndex = index
        root._savedContentY = itunesArtworkResultsView.contentY
        root._savedResultCount = itunesArtworkResultsView.count
        root.uiBridge.prepareItunesArtworkSuggestion(index)
    }

    // Applying overwrites the album's cover, which Ferrous keeps no copy
    // of, so it asks first when the album has one. The suggestion goes to
    // the album it was searched for, which the question names, also if
    // playback moves on while it is asked.
    function applySuggestion(index) {
        const target = root.uiBridge.itunesArtworkTarget || ({})
        if ((target.coverPath || "").length === 0) {
            root.uiBridge.applyItunesArtworkSuggestion(index)
            root.finishAfterApply(index)
            return
        }
        replaceCoverConfirmDialog.suggestionIndex = index
        replaceCoverConfirmDialog.suggestion = root.suggestionRowAt(index)
        // Over the viewer during its preview, else over this dialog.
        replaceCoverConfirmDialog.windowRoot = root.previewShown && root.previewHost && root.previewHost.Window.window
            ? root.previewHost.Window.window : root
        replaceCoverConfirmDialog.message = (target.album || "").length > 0
            ? ((target.artist || "").length > 0
                ? I18n.i18nc("@info album, artist", "Replace the album art of \u201c%1\u201d by %2?",
                    target.album, target.artist)
                : I18n.i18nc("@info", "Replace the album art of \u201c%1\u201d?", target.album))
            : I18n.i18nc("@info", "Replace the current album art with this artwork?")
        replaceCoverConfirmDialog.open()
    }

    function requestSuggestionApply(index) {
        const row = root.suggestionRowAt(index)
        if (root.suggestionRowReady(row)) {
            root.pendingPreviewIndex = -1
            root.pendingApplyIndex = -1
            root.applySuggestion(index)
            return
        }
        root.pendingPreviewIndex = -1
        root.pendingApplyIndex = index
        root._savedContentY = itunesArtworkResultsView.contentY
        root._savedResultCount = itunesArtworkResultsView.count
        root.uiBridge.prepareItunesArtworkSuggestion(index)
    }

    function processPendingSuggestionAction() {
        if (!root.visible) {
            return
        }
        if (root.pendingApplyIndex >= 0) {
            const applyRow = root.suggestionRowAt(root.pendingApplyIndex)
            if (root.suggestionRowReady(applyRow)) {
                const resolvedIndex = root.pendingApplyIndex
                root.clearPendingActionState()
                root.applySuggestion(resolvedIndex)
                return
            }
            if ((((applyRow && (applyRow.assetError || "")) || "").length > 0)
                    && !((applyRow && (applyRow.assetLoading || false)) || false)) {
                root.pendingApplyIndex = -1
            }
        }
        if (root.pendingPreviewIndex >= 0) {
            const previewRow = root.suggestionRowAt(root.pendingPreviewIndex)
            if (root.suggestionRowReady(previewRow)) {
                root.showPreview(root.pendingPreviewIndex)
                return
            }
            if ((((previewRow && (previewRow.assetError || "")) || "").length > 0)
                    && !((previewRow && (previewRow.assetLoading || false)) || false)) {
                root.pendingPreviewIndex = -1
            }
        }
    }

    // Opened anew, also from the viewer's menu during a preview, it has no
    // preview; shown again after one, it already has none.
    onAboutToShow: root.previewShown = false

    onOpenedChanged: {
        if (root.opened) {
            root.clearPendingActionState()
            root.refreshCurrentArtworkInfo()
        }
    }

    // Hidden for a preview, it keeps its results and where it is.
    onClosed: {
        if (!root.previewShown) {
            root.clearAfterClose()
        }
    }

    Connections {
        target: root.uiBridge

        function onItunesArtworkChanged() {
            if (root.previewShown && root.previewIndex >= 0) {
                root.previewRow = root.suggestionRowAt(root.previewIndex)
            }
            root.processPendingSuggestionAction()
            Qt.callLater(() => {
                if (itunesArtworkResultsView.count === root._savedResultCount) {
                    itunesArtworkResultsView.contentY = root._savedContentY
                }
            })
        }

        function onSnapshotChanged() {
            if (root.visible) {
                root.refreshCurrentArtworkInfo()
            }
        }

        function onImageFileDetailsChanged(path) {
            const infoSource = PathUtils.pathFromAnyUrl(root.currentArtworkSource || "")
            if (!root.visible || infoSource.length <= 0 || path !== infoSource) {
                return
            }
            root.currentArtworkInfo = root.uiBridge.cachedImageFileDetails(infoSource) || ({})
        }
    }

    // The rows' grid, shared by the current art and the results: a
    // thumbnail at the search palette's side inset, the text 12 px after
    // it, and the actions at the end. The thumbnail is 64 px, or half as
    // tall again as the title and its line of details with a larger font
    // (a fixed 64 px stood beside 92 px of text at 24 pt).
    readonly property real sideInset: 16
    readonly property real thumbnailSize: Math.max(64, Math.round(1.5 * (2 * lineMetrics.implicitHeight + 2)))
    readonly property real rowPadding: 8
    readonly property real textX: root.sideInset + root.thumbnailSize + 12
    // The actions' column: Preview and Apply 8 px apart, and the text at
    // least 16 px clear of them, in the buttons' own sizes. They are flat
    // push buttons, which the style draws in the general font, as the text
    // and Close: it draws a ToolButton's caption in the toolbar font, which
    // KDE sets apart, so at 24 pt they stayed at 10 pt. Their glyphs are as
    // large as the captions' font calls for, Kirigami's size for labels:
    // 16 px with the default font, larger with a larger one (a fixed 16 px
    // stood beside 24 pt captions).
    readonly property real actionIconSize: root.uiPalette.labelIconSize
    readonly property real actionSpacing: Kirigami.Units.largeSpacing
    readonly property real textActionsGap: 16
    // A row, the current art's or a result's: the thumbnail, or the title
    // and up to two lines of details, or the buttons, whichever is
    // tallest, with 8 px above and below; 80 px with the default font,
    // where the thumbnail is the tallest.
    readonly property real rowHeight: Math.max(root.thumbnailSize, 3 * lineMetrics.implicitHeight + 2,
        applyMetrics.implicitHeight) + 2 * root.rowPadding
    // What the search found, on a line with 8 px above and below.
    readonly property real headingHeight: lineMetrics.implicitHeight + 2 * Kirigami.Units.largeSpacing
    // The results the dialog opens tall enough for; more scroll.
    readonly property int shownResultRows: 5
    // The rows' secondary line: the details of an image, in plain words,
    // such as "1,200 × 1,200 px · JPEG · 412 KiB": the size as the art
    // viewer's details give it, with the locale's thousands separator.
    function imageDetailsText(info) {
        if (!info) {
            return ""
        }
        const width = Number(info.width || 0)
        const height = Number(info.height || 0)
        return [
            width > 0 && height > 0
                ? I18n.i18nc("@info image size, width × height", "%1 × %2 px",
                    width.toLocaleString(Qt.locale(), "f", 0), height.toLocaleString(Qt.locale(), "f", 0))
                : "",
            info.format || info.extension || "",
            root.fileSizeText(info)
        ].filter(Boolean).join(" · ")
    }

    // The file's size, such as "412 KiB", without the exact byte count
    // that the details' fileSizeText adds.
    function fileSizeText(info) {
        const bytes = Number(info.fileSizeBytes)
        if (!(info.fileSizeBytes !== undefined && bytes >= 0)) {
            return info.fileSizeText || ""
        }
        const units = ["B", "KiB", "MiB", "GiB", "TiB"]
        let scaled = bytes
        let unit = 0
        while (scaled >= 1024 && unit < units.length - 1) {
            scaled /= 1024
            ++unit
        }
        return I18n.i18nc("@info file size: size, unit such as MiB", "%1 %2",
            scaled.toLocaleString(Qt.locale(), "f", unit === 0 || scaled >= 10 ? 0 : 1), units[unit])
    }

    // A row's thumbnail: the image fitted in the rows' square, or a quiet
    // placeholder without one.
    component ArtworkThumbnail: Item {
        property alias source: thumbnailImage.source
        // Decoded at the rows' size at any size it is drawn, so one image
        // serves the row and the question's smaller thumbnail of it.

        width: root.thumbnailSize
        height: root.thumbnailSize

        Image {
            id: thumbnailImage
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            sourceSize.width: Math.ceil(root.thumbnailSize * Screen.devicePixelRatio)
            sourceSize.height: Math.ceil(root.thumbnailSize * Screen.devicePixelRatio)
            smooth: true
            asynchronous: true
            cache: true
            visible: source.toString().length > 0
        }

        Kirigami.Icon {
            anchors.centerIn: parent
            width: root.uiPalette.scaledSize(Kirigami.Units.iconSizes.smallMedium)
            height: width
            visible: !thumbnailImage.visible
            source: "image-missing"
            isMask: true
            color: root.uiPalette.textTertiary
        }
    }

    // One case of Replace Album Art: what the folder holds, then what
    // Replace does there.
    component ReplaceCoverCase: ColumnLayout {
        id: replaceCoverCase
        property string lead: ""
        property string outcome: ""
        Layout.fillWidth: true
        spacing: 2

        Label {
            objectName: replaceCoverCase.objectName + "Lead"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.weight: Font.DemiBold
            color: root.uiPalette.text
            textFormat: Text.PlainText
            text: replaceCoverCase.lead
        }

        Label {
            objectName: replaceCoverCase.objectName + "Outcome"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: root.uiPalette.textSecondary
            textFormat: Text.PlainText
            text: replaceCoverCase.outcome
        }
    }

    // A row's text: the title, elided, then the secondary line, which
    // wraps to a second line where it does not fit, as with a large font
    // (the artist and "Loading high-resolution artwork…" elided at
    // 24 pt), and elides past that, with the whole of it in a tooltip.
    component ArtworkText: Column {
        property string title: ""
        // Semibold for the dialog's lead, the current art's; a result's
        // is regular.
        property int titleWeight: Font.Normal
        property string details: ""
        property color detailsColor: root.uiPalette.textSecondary
        property string titleObjectName: ""
        property string detailsObjectName: ""

        spacing: 2

        Label {
            objectName: parent.titleObjectName
            width: parent.width
            textFormat: Text.PlainText
            text: parent.title
            font.weight: parent.titleWeight
            color: root.uiPalette.text
            elide: Text.ElideRight
        }

        Label {
            id: detailsLabel
            objectName: parent.detailsObjectName
            width: parent.width
            visible: text.length > 0
            textFormat: Text.PlainText
            text: parent.details
            color: parent.detailsColor
            font.features: root.uiPalette.tabularFigures
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            ToolTip.text: text
            ToolTip.visible: detailsHover.hovered && truncated
            ToolTip.delay: Kirigami.Units.toolTipDelay

            HoverHandler {
                id: detailsHover
            }
        }
    }

    // Measures Apply, so the current art's Preview lines up with the
    // results' Preview, which Apply follows.
    Button {
        id: applyMetrics
        visible: false
        flat: true
        Kirigami.MnemonicData.enabled: false
        display: AbstractButton.TextBesideIcon
        icon.width: root.actionIconSize
        icon.height: root.actionIconSize
        icon.name: "dialog-ok-apply"
        text: I18n.i18nc("@action:button", "Apply")
    }

    // Measures a line of the rows' text, before there is a row.
    Label {
        id: lineMetrics
        visible: false
        textFormat: Text.PlainText
        text: "X"
    }

    ColumnLayout {
        id: itunesArtworkContent
        objectName: "itunesArtworkContent"
        anchors.fill: parent
        anchors.margins: root.contentPadding
        spacing: 0

        // Decodes the previewed candidate, small, on a worker thread: one
        // that does not decode cannot be applied.
        Image {
            id: previewCandidateProbe
            visible: false
            source: root.previewShown && root.previewIndex >= 0
                ? ((root.previewRow && root.previewRow.normalizedUrl) || "") : ""
            sourceSize.width: 32
            sourceSize.height: 32
            asynchronous: true
            cache: false
        }

        // The current art, on the rows' grid above the results, with no
        // frame of its own: the dialog's first line in every state.
        Item {
            id: currentArtworkRow
            objectName: "itunesArtworkCurrentRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.rowHeight

            ArtworkThumbnail {
                id: currentArtworkThumbnail
                objectName: "itunesArtworkCurrentThumbnail"
                // The frame's 1 px edge, then the results' inset.
                x: 1 + root.sideInset
                y: Math.round((parent.height - height) / 2)
                source: root.currentArtworkSource
            }

            ArtworkText {
                id: currentArtworkText
                x: 1 + root.textX
                y: Math.round((parent.height - height) / 2)
                // The lead in every state, as the other dialogs' first
                // line: what there is, apart from what is offered (it had
                // the weight of every result's title).
                titleWeight: Font.DemiBold
                width: Math.max(0, currentArtworkPreviewButton.x - root.textActionsGap - x)
                titleObjectName: "itunesArtworkCurrentTitle"
                detailsObjectName: "itunesArtworkCurrentDetails"
                title: I18n.i18nc("@label", "Current album art")
                // Under the lead, which has said what it is: neither line
                // says it again.
                details: (root.currentArtworkSource || "").length === 0
                    ? I18n.i18nc("@info after the title Current album art", "This track has no album art.")
                    : (Object.keys(root.currentArtworkInfo || {}).length === 0
                        ? I18n.i18nc("@info after the title Current album art", "Image details aren’t available.")
                        : root.imageDetailsText(root.currentArtworkInfo))
            }

            // In the column of the results' Preview, before Apply's, whether
            // it has rows, searches, found nothing or failed: a button that
            // stays where it is as the search ends (with no rows it went to
            // the content's edge, 109 px, in one frame).
            Button {
                id: currentArtworkPreviewButton
                objectName: "itunesArtworkCurrentPreviewButton"
                // No Alt shortcut: the rows repeat Preview and Apply, which would
                // each take another letter, and a hidden button has none, so
                // Apply's measure would differ from the rows' by the mnemonic.
                Kirigami.MnemonicData.enabled: false
                x: parent.width - 1 - root.sideInset - applyMetrics.implicitWidth - root.actionSpacing - width
                y: Math.round((parent.height - height) / 2)
                flat: true
                display: AbstractButton.TextBesideIcon
                icon.width: root.actionIconSize
                icon.height: root.actionIconSize
                icon.name: "view-preview"
                text: I18n.i18nc("@action:button", "Preview")
                // Hidden, it takes no click: one more, as it hides, would
                // act again.
                enabled: (root.currentArtworkSource || "").length > 0 && root.opened
                onClicked: root.showPreview(-1)
            }
        }

        // What the search found, over the results it is about: on the rows'
        // inset, 8 px clear of the current art above and the results' frame
        // below.
        Label {
            objectName: "itunesArtworkResultsHeading"
            Layout.fillWidth: true
            Layout.leftMargin: 1 + root.sideInset
            Layout.rightMargin: 1 + root.sideInset
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            visible: itunesArtworkResultsView.count > 0 && text.length > 0
            textFormat: Text.PlainText
            text: root.uiBridge.itunesArtworkStatusText || ""
            color: root.uiPalette.textSecondary
            wrapMode: Text.Wrap
        }

        // The results: a flat list on the view colour, in the view's frame,
        // with a hairline between rows. The frame ends at the last row, and
        // the window's room below the rows stays its own colour; with more
        // rows than room, they scroll. Searching, or with nothing found,
        // there is no frame, only a placeholder message on the dialog's own
        // colour (rather than an empty well framing the message).
        Rectangle {
            id: resultsFrame
            objectName: "itunesArtworkResultsFrame"
            readonly property bool framed: itunesArtworkResultsView.count > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: resultsFrame.framed ? resultsFrame.Layout.maximumHeight : 0
            Layout.maximumHeight: resultsFrame.framed
                ? itunesArtworkResultsView.count * root.rowHeight + 2 : Number.POSITIVE_INFINITY
            Layout.topMargin: resultsFrame.framed ? 0 : Kirigami.Units.smallSpacing
            color: resultsFrame.framed ? root.uiPalette.view : "transparent"
            border.width: resultsFrame.framed ? 1 : 0
            border.color: root.uiPalette.seam

            ListView {
                id: itunesArtworkResultsView
                objectName: "itunesArtworkResultsView"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                model: root.uiBridge.itunesArtworkResults
                visible: count > 0

                ScrollBar.vertical: Components.LaneScrollBar {
                    id: itunesArtworkScrollBar
                    uiPalette: root.uiPalette
                }

                delegate: Item {
                    id: suggestionRow
                    required property int index
                    required property var modelData
                    readonly property Item previewButton: suggestionPreviewButton
                    readonly property bool assetLoading: (modelData && (modelData.assetLoading || false)) || false
                    readonly property string assetError: (modelData && (modelData.assetError || "")) || ""
                    readonly property string artistName: (modelData && (modelData.artistName || "")) || ""

                    objectName: "itunesArtworkResultRow"
                    width: ListView.view.width
                    height: root.rowHeight

                    ArtworkThumbnail {
                        objectName: "itunesArtworkResultThumbnail"
                        x: root.sideInset
                        y: Math.round((parent.height - height) / 2)
                        source: (suggestionRow.modelData && (suggestionRow.modelData.previewSource || "")) || ""
                    }

                    // The secondary line: the artist and the image's
                    // details; while the full-size image loads, that;
                    // when it failed, why.
                    ArtworkText {
                        id: suggestionText
                        x: root.textX
                        y: Math.round((parent.height - height) / 2)
                        width: Math.max(0, suggestionActions.x - root.textActionsGap - x)
                        titleObjectName: "itunesArtworkResultTitle"
                        detailsObjectName: "itunesArtworkResultDetails"
                        title: (suggestionRow.modelData && (suggestionRow.modelData.albumTitle || "")) || ""
                        details: suggestionRow.assetError.length > 0 && !suggestionRow.assetLoading
                            ? suggestionRow.assetError
                            : [suggestionRow.artistName, suggestionRow.assetLoading
                                ? I18n.i18nc("@label", "Loading full-size image…")
                                : root.imageDetailsText(suggestionRow.modelData)]
                                .filter(Boolean).join(" · ")
                        detailsColor: suggestionRow.assetError.length > 0 && !suggestionRow.assetLoading
                            ? root.uiPalette.negative : root.uiPalette.textSecondary
                    }

                    // Preview, then Apply, the row's main action, at its end:
                    // on the rows' inset, which holds the scroll bar's lane
                    // whether the bar shows or not, so a sixth result moves
                    // no column.
                    Row {
                        id: suggestionActions
                        objectName: "itunesArtworkResultActions"
                        x: parent.width - root.sideInset - width
                        y: Math.round((parent.height - height) / 2)
                        spacing: root.actionSpacing

                        Button {
                            id: suggestionPreviewButton
                            objectName: "itunesArtworkPreviewButton"
                            Kirigami.MnemonicData.enabled: false
                            flat: true
                            display: AbstractButton.TextBesideIcon
                            icon.width: root.actionIconSize
                            icon.height: root.actionIconSize
                            icon.name: "view-preview"
                            text: I18n.i18nc("@action:button", "Preview")
                            enabled: !suggestionRow.assetLoading && root.opened
                            onClicked: root.requestSuggestionPreview(suggestionRow.index)
                        }

                        Button {
                            id: suggestionApplyButton
                            objectName: "itunesArtworkApplyButton"
                            Kirigami.MnemonicData.enabled: false
                            flat: true
                            display: AbstractButton.TextBesideIcon
                            icon.width: root.actionIconSize
                            icon.height: root.actionIconSize
                            icon.name: "dialog-ok-apply"
                            text: I18n.i18nc("@action:button", "Apply")
                            enabled: !suggestionRow.assetLoading && root.opened
                            onClicked: root.requestSuggestionApply(suggestionRow.index)
                        }
                    }

                    // The hairline to the next row, from the text's left edge
                    // to the row's end, so the thumbnails stand in a column
                    // of their own (not under them, from the frame's
                    // edge).
                    Rectangle {
                        objectName: "itunesArtworkResultHairline"
                        visible: suggestionRow.index < itunesArtworkResultsView.count - 1
                        anchors.left: parent.left
                        anchors.leftMargin: root.textX
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: root.uiPalette.hairline
                    }
                }
            }

            // Searching, or nothing found: what the search says, a little
            // above the middle of where the results would be, under a slot
            // with the busy indicator while it searches. The slot stays, so
            // the first line keeps its baseline as the search ends, and the
            // lines after it grow downwards. Finding nothing is a routine
            // outcome, and speaks as searching does: a glyph in the slot and
            // a line in the secondary colour, with no heading or frame (a
            // line in the text's colour under an empty slot, it outranked
            // searching). A search that failed says so in a
            // semibold heading, with the system's words under it in the
            // secondary colour (otherwise it read as finding nothing),
            // and a warning glyph in the negative colour in the spinner's
            // place. Built from its
            // parts: Kirigami.PlaceholderMessage's selectable text brings
            // Copy and Select All actions that would take Ctrl+C and Ctrl+A
            // from the Edit menu's.
            ColumnLayout {
                id: placeholder
                objectName: "itunesArtworkPlaceholder"
                readonly property bool searching: root.uiBridge.itunesArtworkLoading === true
                readonly property bool failed: !placeholder.searching && root.uiBridge.itunesArtworkFailed === true
                // The slot and one line: what is placed, whatever follows
                // the line (centred whole, finding nothing was 22 px above
                // searching, and the failure's heading 11 px).
                readonly property real firstLineBottom: glyphSlot.Layout.preferredHeight + placeholder.spacing
                    + lineMetrics.implicitHeight
                anchors.horizontalCenter: parent.horizontalCenter
                // 3 grid units above the middle, room permitting.
                y: Math.round(Math.max(0, Math.min(parent.height - placeholder.height,
                    (parent.height - placeholder.firstLineBottom) / 2 - 3 * Kirigami.Units.gridUnit)))
                width: Math.min(parent.width - 4 * Kirigami.Units.gridUnit, root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 24))
                spacing: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                visible: itunesArtworkResultsView.count === 0
                    && (placeholder.searching || placeholderMessage.text.length > 0)

                // The state's one glyph: 32 px with the default font and
                // larger with a larger one, as the text under it (a fixed
                // 32 px stood over 24 pt text).
                Item {
                    id: glyphSlot
                    objectName: "itunesArtworkGlyphSlot"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: root.uiPalette.scaledSize(Kirigami.Units.iconSizes.medium)
                    Layout.preferredHeight: root.uiPalette.scaledSize(Kirigami.Units.iconSizes.medium)

                    // The style's spinner.
                    BusyIndicator {
                        objectName: "itunesArtworkBusyIndicator"
                        anchors.fill: parent
                        running: placeholder.searching
                        visible: running
                    }

                    // A failed search's glyph, in the spinner's place and
                    // size: the app's own warning mark, as the status bar
                    // and the queue draw it (Breeze's dialog-warning as a
                    // mask fills its "!", leaving a plain triangle).
                    Kirigami.Icon {
                        objectName: "itunesArtworkFailureIcon"
                        anchors.fill: parent
                        visible: placeholder.failed
                        source: Qt.resolvedUrl("../assets/status-warning.svg")
                        isMask: true
                        color: root.uiPalette.negative
                        Accessible.ignored: true
                    }

                    // Nothing to show: the search's glyph, in the same
                    // place, size and ink.
                    Kirigami.Icon {
                        objectName: "itunesArtworkNothingFoundIcon"
                        anchors.fill: parent
                        visible: !placeholder.searching && !placeholder.failed
                        source: "edit-find"
                        isMask: true
                        color: root.uiPalette.textSecondary
                        Accessible.ignored: true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Label {
                        objectName: "itunesArtworkFailureHeading"
                        Layout.fillWidth: true
                        visible: placeholder.failed
                        textFormat: Text.PlainText
                        text: I18n.i18nc("@info", "Couldn\u2019t search iTunes")
                        font.weight: Font.DemiBold
                        color: root.uiPalette.text
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        // The status in a sentence, as the bridge has it.
                        Accessible.description: root.uiBridge.itunesArtworkStatusText || ""
                    }

                    Label {
                        id: placeholderMessage
                        objectName: "itunesArtworkPlaceholderMessage"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        textFormat: Text.PlainText
                        text: placeholder.failed
                            ? (root.uiBridge.itunesArtworkErrorText || "")
                            : (root.uiBridge.itunesArtworkStatusText || "")
                        color: root.uiPalette.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        // The room under the results' frame, when they leave some.
        Item {
            objectName: "itunesArtworkResultsRoom"
            visible: resultsFrame.framed
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }

    Dialogs.ConfirmDialog {
        id: replaceCoverConfirmDialog
        objectName: "replaceCoverConfirmDialog"
        property int suggestionIndex: -1
        // The suggestion's row, as it was resolved when the question asked.
        property var suggestion: ({})
        // Over this dialog, or over the viewer during its preview: see
        // applySuggestion().
        uiPalette: root.uiPalette
        windowRoot: root
        title: I18n.i18nc("@title:window", "Replace Album Art")

        // What was chosen, under the question, which covers the row it
        // came from: that row's thumbnail, title and artist. Display only.
        RowLayout {
            objectName: "replaceCoverChosen"
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing

            ArtworkThumbnail {
                objectName: "replaceCoverChosenThumbnail"
                readonly property int side: root.uiPalette.scaledSize(48)
                Layout.preferredWidth: side
                Layout.preferredHeight: side
                source: replaceCoverConfirmDialog.suggestion.previewSource || ""
                Accessible.ignored: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    objectName: "replaceCoverChosenTitle"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    text: replaceCoverConfirmDialog.suggestion.albumTitle || ""
                    color: root.uiPalette.text
                    wrapMode: Text.Wrap
                }

                Label {
                    objectName: "replaceCoverChosenArtist"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    text: replaceCoverConfirmDialog.suggestion.artistName || ""
                    color: root.uiPalette.textSecondary
                    wrapMode: Text.Wrap
                }
            }
        }

        // Each case on a row of its own, as the backend applies it
        // (artwork::apply_artwork_to_track): what the folder holds, in the
        // text's ink and semibold, then what Replace does there, secondary,
        // so the reader finds the case that applies without parsing one
        // paragraph of conditions (it was one, read twice). All of it is in
        // the folder of the track the art was searched for. A folder with a
        // JPEG or PNG gets a cover file, as Ferrous shows any such image as
        // the folder's cover (folder_cover), a back scan alone too, which
        // nobody would call "a cover image": the one with a cover's name is
        // overwritten, and otherwise a new one is written beside the images
        // that stay, so not every replaced image is lost ("replaces the
        // album's cover image file" said it was). Without such an image,
        // the tracks in that folder with the album's tag get it as their
        // front cover, and no other folder's. It says which file is
        // overwritten ("becomes its cover file" left that open, beside a
        // warning that something is lost).
        ColumnLayout {
            objectName: "replaceCoverConsequence"
            Layout.fillWidth: true
            // 6 px between the rows with the default font, more with a
            // larger one; a row's lead and outcome sit as close as a
            // question and its path.
            spacing: root.uiPalette.scaledSize(Kirigami.Units.smallSpacing + Kirigami.Units.smallSpacing / 2)

            ReplaceCoverCase {
                objectName: "replaceCoverFolderCase"
                lead: I18n.i18nc("@info", "Folder has JPEG or PNG images")
                outcome: I18n.i18nc("@info", "Overwrites the image named as a cover, or adds a cover file "
                    + "beside them")
            }

            ReplaceCoverCase {
                objectName: "replaceCoverEmbedCase"
                lead: I18n.i18nc("@info", "No JPEG or PNG images in the folder")
                outcome: I18n.i18nc("@info", "Embeds the album art in this album’s tracks in that folder")
            }
        }

        // What is lost, on its own line and in the text's colour (it ended
        // the paragraph above, the last thing read).
        Label {
            objectName: "replaceCoverWarning"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: root.uiPalette.text
            textFormat: Text.PlainText
            text: I18n.i18nc("@info", "Ferrous keeps no copy of what it overwrites.")
        }

        confirmText: I18n.i18nc("@action:button", "Replace")
        // Breeze's swap: one image for the other. Its "document-replace", a
        // page with a red ×, read as Delete; red is the glyph of the
        // confirmations that delete or remove. The question and Cancel as
        // the default say that the current art is lost.
        confirmIcon: "document-swap"
        // The current album art is gone once replaced.
        destructive: true
        // The answer takes the suggestion it was asked for, at once.
        onConfirmed: {
            const index = suggestionIndex
            suggestionIndex = -1
            if (index < 0) {
                return
            }
            root.uiBridge.applyItunesArtworkSuggestion(index)
            root.finishAfterApply(index)
        }
    }
}
