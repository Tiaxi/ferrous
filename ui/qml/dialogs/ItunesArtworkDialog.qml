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
    // 920 by 680 px with the default font, and as much larger as a larger
    // font makes the rows, within the screen; the user may resize it.
    defaultWidth: root.uiPalette.scaledSize(920)
    defaultHeight: root.uiPalette.scaledSize(680)
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

    // The rows' grid, shared by the current art and the results: a 64 px
    // thumbnail at the search palette's side inset, the text 12 px after
    // it, and the actions at the end.
    readonly property real sideInset: 16
    readonly property real thumbnailSize: 64
    readonly property real rowPadding: 8
    readonly property real textX: root.sideInset + root.thumbnailSize + 12
    // The rows' secondary line: the details of an image, in plain words,
    // such as "1200 × 1200 · JPEG · 412 KiB".
    function imageDetailsText(info) {
        if (!info) {
            return ""
        }
        const width = Number(info.width || 0)
        const height = Number(info.height || 0)
        return [
            width > 0 && height > 0
                ? I18n.i18nc("@info image size in pixels, width × height", "%1 × %2",
                    String(width), String(height))
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

    // A row's thumbnail: the image fitted in a 64 px square, or a quiet
    // placeholder without one.
    component ArtworkThumbnail: Item {
        property alias source: thumbnailImage.source

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
            width: Kirigami.Units.iconSizes.smallMedium
            height: width
            visible: !thumbnailImage.visible
            source: "image-missing"
            isMask: true
            color: root.uiPalette.textTertiary
        }
    }

    // A row's text: the title, then one secondary line, elided, with the
    // whole of it in a tooltip when it does not fit.
    component ArtworkText: Column {
        property string title: ""
        property string details: ""
        property color detailsColor: root.uiPalette.textSecondary
        property string titleObjectName: ""
        property string detailsObjectName: ""

        spacing: 2

        Label {
            objectName: parent.titleObjectName
            width: parent.width
            text: parent.title
            color: root.uiPalette.text
            elide: Text.ElideRight
        }

        Label {
            id: detailsLabel
            objectName: parent.detailsObjectName
            width: parent.width
            visible: text.length > 0
            text: parent.details
            color: parent.detailsColor
            font.features: root.uiPalette.tabularFigures
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
    ToolButton {
        id: applyMetrics
        visible: false
        flat: true
        display: AbstractButton.TextBesideIcon
        icon.name: "dialog-ok-apply"
        text: I18n.i18nc("@action:button", "Apply")
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
            Layout.preferredHeight: Math.max(root.thumbnailSize,
                currentArtworkText.implicitHeight, currentArtworkPreviewButton.implicitHeight)
                + 2 * root.rowPadding

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
                width: Math.max(0, currentArtworkPreviewButton.x - 12 - x)
                titleObjectName: "itunesArtworkCurrentTitle"
                detailsObjectName: "itunesArtworkCurrentDetails"
                title: I18n.i18nc("@label", "Current album art")
                details: (root.currentArtworkSource || "").length === 0
                    ? I18n.i18nc("@label", "No current album art is available for this track.")
                    : (Object.keys(root.currentArtworkInfo || {}).length === 0
                        ? I18n.i18nc("@label", "Current artwork metadata is not available.")
                        : root.imageDetailsText(root.currentArtworkInfo))
            }

            // Where the results' Preview is: before Apply, clear of the
            // scrollbar lane.
            ToolButton {
                id: currentArtworkPreviewButton
                objectName: "itunesArtworkCurrentPreviewButton"
                x: parent.width - 1 - itunesArtworkResultsView.scrollbarLane - root.sideInset
                    - applyMetrics.implicitWidth - Kirigami.Units.smallSpacing - width
                y: Math.round((parent.height - height) / 2)
                flat: true
                display: AbstractButton.TextBesideIcon
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
            text: root.uiBridge.itunesArtworkStatusText || ""
            color: root.uiPalette.textSecondary
            wrapMode: Text.Wrap
        }

        // The results: a flat list on the view colour, in the view's frame,
        // with a hairline between rows. Searching, or with nothing found,
        // there is no frame, only a placeholder message on the dialog's own
        // colour (#373 H8: an empty well framed the message).
        Rectangle {
            id: resultsFrame
            objectName: "itunesArtworkResultsFrame"
            readonly property bool framed: itunesArtworkResultsView.count > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: resultsFrame.framed ? 0 : Kirigami.Units.smallSpacing
            color: resultsFrame.framed ? root.uiPalette.view : "transparent"
            border.width: resultsFrame.framed ? 1 : 0
            border.color: root.uiPalette.seam

            ListView {
                id: itunesArtworkResultsView
                objectName: "itunesArtworkResultsView"
                // The lane the rows leave for the scrollbar, when it shows.
                readonly property real scrollbarLane: itunesArtworkScrollBar.visible
                    ? itunesArtworkScrollBar.lane : 0
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
                    height: Math.max(root.thumbnailSize, suggestionText.implicitHeight,
                        suggestionApplyButton.implicitHeight) + 2 * root.rowPadding

                    ArtworkThumbnail {
                        objectName: "itunesArtworkResultThumbnail"
                        x: root.sideInset
                        y: Math.round((parent.height - height) / 2)
                        source: (suggestionRow.modelData && (suggestionRow.modelData.previewSource || "")) || ""
                    }

                    // One secondary line: the artist and the image's
                    // details; while the high-resolution image loads, that;
                    // when it failed, why.
                    ArtworkText {
                        id: suggestionText
                        x: root.textX
                        y: Math.round((parent.height - height) / 2)
                        width: Math.max(0, suggestionActions.x - 12 - x)
                        titleObjectName: "itunesArtworkResultTitle"
                        detailsObjectName: "itunesArtworkResultDetails"
                        title: (suggestionRow.modelData && (suggestionRow.modelData.albumTitle || "")) || ""
                        details: suggestionRow.assetError.length > 0 && !suggestionRow.assetLoading
                            ? suggestionRow.assetError
                            : [suggestionRow.artistName, suggestionRow.assetLoading
                                ? I18n.i18nc("@label", "Loading high-resolution artwork…")
                                : root.imageDetailsText(suggestionRow.modelData)]
                                .filter(Boolean).join(" · ")
                        detailsColor: suggestionRow.assetError.length > 0 && !suggestionRow.assetLoading
                            ? root.uiPalette.negative : root.uiPalette.textSecondary
                    }

                    // Preview, then Apply, the row's main action, at its end.
                    Row {
                        id: suggestionActions
                        objectName: "itunesArtworkResultActions"
                        x: parent.width - itunesArtworkResultsView.scrollbarLane - root.sideInset - width
                        y: Math.round((parent.height - height) / 2)
                        spacing: Kirigami.Units.smallSpacing

                        ToolButton {
                            id: suggestionPreviewButton
                            objectName: "itunesArtworkPreviewButton"
                            flat: true
                            display: AbstractButton.TextBesideIcon
                            icon.name: "view-preview"
                            text: I18n.i18nc("@action:button", "Preview")
                            enabled: !suggestionRow.assetLoading && root.opened
                            onClicked: root.requestSuggestionPreview(suggestionRow.index)
                        }

                        ToolButton {
                            id: suggestionApplyButton
                            objectName: "itunesArtworkApplyButton"
                            flat: true
                            display: AbstractButton.TextBesideIcon
                            icon.name: "dialog-ok-apply"
                            text: I18n.i18nc("@action:button", "Apply")
                            enabled: !suggestionRow.assetLoading && root.opened
                            onClicked: root.requestSuggestionApply(suggestionRow.index)
                        }
                    }

                    // The hairline to the next row.
                    Rectangle {
                        objectName: "itunesArtworkResultHairline"
                        visible: suggestionRow.index < itunesArtworkResultsView.count - 1
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: root.uiPalette.hairline
                    }
                }
            }

            // Searching, or nothing found: what the search says, centred
            // where the results would be, with a busy indicator above it
            // while it searches. Quiet, as the queue's empty state: the base
            // font in the secondary colour, with no glyph, for finding
            // nothing is a routine outcome (#373 H7, H8). Built from its
            // parts: Kirigami.PlaceholderMessage's selectable text brings
            // Copy and Select All actions that would take Ctrl+C and Ctrl+A
            // from the Edit menu's.
            ColumnLayout {
                objectName: "itunesArtworkPlaceholder"
                anchors.centerIn: parent
                width: Math.min(parent.width - 4 * Kirigami.Units.gridUnit, Kirigami.Units.gridUnit * 24)
                spacing: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                visible: itunesArtworkResultsView.count === 0
                    && (root.uiBridge.itunesArtworkLoading || placeholderMessage.text.length > 0)

                // The style's spinner, 32 px: the state's one glyph.
                BusyIndicator {
                    objectName: "itunesArtworkBusyIndicator"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    running: root.uiBridge.itunesArtworkLoading
                    visible: running
                }

                Label {
                    id: placeholderMessage
                    objectName: "itunesArtworkPlaceholderMessage"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.uiBridge.itunesArtworkStatusText || ""
                    color: root.uiPalette.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    Dialogs.ConfirmDialog {
        id: replaceCoverConfirmDialog
        objectName: "replaceCoverConfirmDialog"
        property int suggestionIndex: -1
        // Over this dialog, or over the viewer during its preview: see
        // applySuggestion().
        uiPalette: root.uiPalette
        windowRoot: root
        title: I18n.i18nc("@title:window", "Replace Album Art")
        details: I18n.i18nc("@info", "Ferrous saves the new artwork as the album's cover image, replacing a cover "
            + "image that is there, or embeds it in the album's tracks in place of their artwork. "
            + "It keeps no copy of the current album art.")
        confirmText: I18n.i18nc("@action:button", "Replace")
        confirmIcon: "document-replace"
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
