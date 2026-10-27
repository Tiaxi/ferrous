// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/PathUtils.js" as PathUtils
import "." as Dialogs

Dialog {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var windowRoot
    required property var openAlbumArtViewerForSuggestion
    required property var openAlbumArtViewerForCurrentArt

    property int pendingPreviewIndex: -1
    property int pendingApplyIndex: -1
    property string currentArtworkSource: ""
    property var currentArtworkInfo: ({})
    property real _savedContentY: 0
    property int _savedResultCount: 0
    readonly property real hostWidth: (parent && parent.width > 0) ? parent.width : root.windowRoot.width
    readonly property real hostHeight: (parent && parent.height > 0) ? parent.height : root.windowRoot.height

    parent: Overlay.overlay
    modal: true
    focus: true
    z: 100
    width: Math.min(Math.max(320, hostWidth - 48), 920)
    height: Math.min(Math.max(320, hostHeight - 48), 680)
    x: (hostWidth - width) / 2
    y: (hostHeight - height) / 2
    title: I18n.i18nc("@title:window", "Replace From iTunes")
    standardButtons: Dialog.Close

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

    function requestSuggestionPreview(index) {
        const row = root.suggestionRowAt(index)
        if (root.suggestionRowReady(row)) {
            root.pendingPreviewIndex = -1
            root.pendingApplyIndex = -1
            root.openAlbumArtViewerForSuggestion(row)
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
            root.close()
            return
        }
        replaceCoverConfirmDialog.suggestionIndex = index
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
                root.clearPendingActionState()
                root.openAlbumArtViewerForSuggestion(previewRow)
                return
            }
            if ((((previewRow && (previewRow.assetError || "")) || "").length > 0)
                    && !((previewRow && (previewRow.assetLoading || false)) || false)) {
                root.pendingPreviewIndex = -1
            }
        }
    }

    onOpened: {
        root.clearPendingActionState()
        root.refreshCurrentArtworkInfo()
    }

    onClosed: {
        root.clearPendingActionState()
        // Its suggestion goes with the suggestions cleared here.
        replaceCoverConfirmDialog.close()
        root.uiBridge.clearItunesArtworkSuggestions()
        root.parent = Overlay.overlay
    }

    Connections {
        target: root.uiBridge

        function onItunesArtworkChanged() {
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

    contentItem: ColumnLayout {
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            BusyIndicator {
                running: root.uiBridge.itunesArtworkLoading
                visible: running
            }

            Text {
                Layout.fillWidth: true
                text: root.uiBridge.itunesArtworkStatusText || ""
                color: root.uiPalette.uiTextColor
                wrapMode: Text.Wrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            implicitHeight: currentArtworkSummaryRow.implicitHeight + 20
            radius: 12
            color: root.uiPalette.uiPaneColor
            border.color: root.uiPalette.uiBorderColor
            clip: true

            RowLayout {
                id: currentArtworkSummaryRow
                anchors.fill: parent
                anchors.margins: 10
                spacing: 12

                Item {
                    Layout.preferredWidth: 92
                    Layout.preferredHeight: 92
                    clip: true

                    Image {
                        id: currentArtworkImage
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectFit
                        source: root.currentArtworkSource
                        smooth: true
                        asynchronous: true
                        cache: true
                        visible: (root.currentArtworkSource || "").length > 0
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: 10
                        color: root.uiPalette.uiSurfaceAltColor
                        border.color: root.uiPalette.uiBorderColor
                        visible: !currentArtworkImage.visible

                        Text {
                            anchors.centerIn: parent
                            text: I18n.i18nc("@label", "No art")
                            color: root.uiPalette.uiMutedTextColor
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text {
                        Layout.fillWidth: true
                        text: I18n.i18nc("@label", "Current album art")
                        color: root.uiPalette.uiTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.2
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.fillWidth: true
                        text: [
                            (root.currentArtworkInfo.resolutionText || ""),
                            (root.currentArtworkInfo.fileType || ""),
                            (root.currentArtworkInfo.fileSizeText || "")
                        ].filter(Boolean).join("  |  ")
                        color: root.uiPalette.uiMutedTextColor
                        wrapMode: Text.Wrap
                        visible: text.length > 0
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: (root.currentArtworkInfo.mimeType || "").length > 0
                        text: I18n.i18nc("@info", "MIME: %1", root.currentArtworkInfo.mimeType || "")
                        color: root.uiPalette.uiMutedTextColor
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: (root.currentArtworkSource || "").length === 0
                        text: I18n.i18nc("@label", "No current album art is available for this track.")
                        color: root.uiPalette.uiMutedTextColor
                        wrapMode: Text.Wrap
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: (root.currentArtworkSource || "").length > 0
                            && Object.keys(root.currentArtworkInfo || {}).length === 0
                        text: I18n.i18nc("@label", "Current artwork metadata is not available.")
                        color: root.uiPalette.uiMutedTextColor
                        wrapMode: Text.Wrap
                    }
                }

                Button {
                    text: I18n.i18nc("@action:button", "Preview")
                    enabled: (root.currentArtworkSource || "").length > 0
                    onClicked: root.openAlbumArtViewerForCurrentArt()
                }
            }
        }

        ListView {
            id: itunesArtworkResultsView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 10
            boundsBehavior: Flickable.StopAtBounds
            rightMargin: itunesArtworkScrollBar.visible ? (itunesArtworkScrollBar.width + 10) : 0
            model: root.uiBridge.itunesArtworkResults
            visible: count > 0

            ScrollBar.vertical: ScrollBar {
                id: itunesArtworkScrollBar
                policy: ScrollBar.AsNeeded
            }

            delegate: Rectangle {
                required property int index
                required property var modelData

                x: ListView.view.leftMargin
                width: Math.max(0, ListView.view.width - ListView.view.leftMargin - ListView.view.rightMargin)
                implicitHeight: 136
                radius: 12
                color: root.uiPalette.uiPaneColor
                border.color: root.uiPalette.uiBorderColor

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 12

                    Image {
                        Layout.preferredWidth: 92
                        Layout.preferredHeight: 92
                        fillMode: Image.PreserveAspectFit
                        source: (modelData && (modelData.previewSource || "")) || ""
                        smooth: true
                        asynchronous: true
                        cache: true
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Text {
                            Layout.fillWidth: true
                            text: (modelData && (modelData.albumTitle || "")) || ""
                            color: root.uiPalette.uiTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.2
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }

                        Text {
                            Layout.fillWidth: true
                            text: (modelData && (modelData.artistName || "")) || ""
                            color: root.uiPalette.uiMutedTextColor
                            elide: Text.ElideRight
                        }

                        Text {
                            Layout.fillWidth: true
                            text: [
                                (modelData && (modelData.resolutionText || "")) || "",
                                (modelData && (modelData.fileType || "")) || "",
                                (modelData && (modelData.fileSizeText || "")) || ""
                            ].filter(Boolean).join("  |  ")
                            color: root.uiPalette.uiMutedTextColor
                            wrapMode: Text.Wrap
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: ((modelData && (modelData.mimeType || "")) || "").length > 0
                            text: I18n.i18nc("@info", "MIME: %1", (modelData && (modelData.mimeType || "")) || "")
                            color: root.uiPalette.uiMutedTextColor
                            elide: Text.ElideRight
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: ((modelData && (modelData.assetLoading || false)) || false)
                            spacing: 8

                            BusyIndicator {
                                Layout.preferredWidth: 18
                                Layout.preferredHeight: 18
                                running: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: I18n.i18nc("@label", "Loading high-resolution artwork...")
                                color: root.uiPalette.uiMutedTextColor
                                wrapMode: Text.Wrap
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: ((modelData && (modelData.assetError || "")) || "").length > 0
                            text: (modelData && (modelData.assetError || "")) || ""
                            color: Kirigami.Theme.negativeTextColor
                            wrapMode: Text.Wrap
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: !((modelData && (modelData.assetLoading || false)) || false)
                                && ((modelData && (modelData.detailStatusText || "")) || "").length > 0
                            text: (modelData && (modelData.detailStatusText || "")) || ""
                            color: root.uiPalette.uiMutedTextColor
                            wrapMode: Text.Wrap
                        }
                    }

                    ColumnLayout {
                        spacing: 8

                        Button {
                            text: ((modelData && (modelData.assetLoading || false)) || false)
                                && root.pendingPreviewIndex === index
                                ? I18n.i18nc("@action:button", "Loading...")
                                : I18n.i18nc("@action:button", "Preview")
                            enabled: !((modelData && (modelData.assetLoading || false)) || false)
                            onClicked: root.requestSuggestionPreview(index)
                        }

                        Button {
                            text: ((modelData && (modelData.assetLoading || false)) || false)
                                && root.pendingApplyIndex === index
                                ? I18n.i18nc("@action:button", "Loading...")
                                : I18n.i18nc("@action:button", "Apply")
                            enabled: !((modelData && (modelData.assetLoading || false)) || false)
                            onClicked: root.requestSuggestionApply(index)
                        }
                    }
                }
            }
        }
    }

    Dialogs.ConfirmDialog {
        id: replaceCoverConfirmDialog
        objectName: "replaceCoverConfirmDialog"
        property int suggestionIndex: -1
        // Over the viewer that holds this dialog, when one does.
        parent: root.parent
        uiPalette: root.uiPalette
        windowRoot: root.windowRoot
        title: I18n.i18nc("@title:window", "Replace Album Art")
        details: I18n.i18nc("@info", "Ferrous saves the new artwork as the album's cover image, replacing a cover "
            + "image that is there, or embeds it in the album's tracks in place of their artwork. "
            + "It keeps no copy of the current album art.")
        confirmText: I18n.i18nc("@action:button", "Replace")
        onConfirmed: {
            root.uiBridge.applyItunesArtworkSuggestion(suggestionIndex)
            root.close()
        }
    }
}
