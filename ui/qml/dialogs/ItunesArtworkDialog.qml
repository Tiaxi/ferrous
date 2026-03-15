import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

Dialog {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var windowRoot
    required property var pathFromAnyUrl
    required property var openAlbumArtViewerForSuggestion

    property int pendingPreviewIndex: -1
    property int pendingApplyIndex: -1
    property string currentArtworkSource: ""
    property var currentArtworkInfo: ({})
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
    title: "Replace From iTunes"
    standardButtons: Dialog.Close

    function clearPendingActionState() {
        root.pendingPreviewIndex = -1
        root.pendingApplyIndex = -1
    }

    function refreshCurrentArtworkInfo() {
        root.currentArtworkSource = root.uiBridge.currentTrackCoverPath || ""
        const infoSource = root.pathFromAnyUrl(root.currentArtworkSource)
        root.currentArtworkInfo = infoSource.length > 0
            ? (root.uiBridge.imageFileDetails(infoSource) || ({}))
            : ({})
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
        root.uiBridge.prepareItunesArtworkSuggestion(index)
    }

    function requestSuggestionApply(index) {
        const row = root.suggestionRowAt(index)
        if (root.suggestionRowReady(row)) {
            root.pendingPreviewIndex = -1
            root.pendingApplyIndex = -1
            root.uiBridge.applyItunesArtworkSuggestion(index)
            root.close()
            return
        }
        root.pendingPreviewIndex = -1
        root.pendingApplyIndex = index
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
                root.uiBridge.applyItunesArtworkSuggestion(resolvedIndex)
                root.close()
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
        root.uiBridge.clearItunesArtworkSuggestions()
        root.parent = Overlay.overlay
    }

    Connections {
        target: root.uiBridge

        function onItunesArtworkChanged() {
            root.processPendingSuggestionAction()
        }

        function onSnapshotChanged() {
            if (root.visible) {
                root.refreshCurrentArtworkInfo()
            }
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
                            text: "No art"
                            color: root.uiPalette.uiMutedTextColor
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text {
                        Layout.fillWidth: true
                        text: "Current album art"
                        color: root.uiPalette.uiTextColor
                        font.pixelSize: 16
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
                        text: "MIME: " + (root.currentArtworkInfo.mimeType || "")
                        color: root.uiPalette.uiMutedTextColor
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: (root.currentArtworkSource || "").length === 0
                        text: "No current album art is available for this track."
                        color: root.uiPalette.uiMutedTextColor
                        wrapMode: Text.Wrap
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: (root.currentArtworkSource || "").length > 0
                            && Object.keys(root.currentArtworkInfo || {}).length === 0
                        text: "Current artwork metadata is not available."
                        color: root.uiPalette.uiMutedTextColor
                        wrapMode: Text.Wrap
                    }
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
                            font.pixelSize: 16
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
                            text: "MIME: " + (((modelData && (modelData.mimeType || "")) || ""))
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
                                text: "Loading high-resolution artwork..."
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
                                ? "Loading..."
                                : "Preview"
                            enabled: !((modelData && (modelData.assetLoading || false)) || false)
                            onClicked: root.requestSuggestionPreview(index)
                        }

                        Button {
                            text: ((modelData && (modelData.assetLoading || false)) || false)
                                && root.pendingApplyIndex === index
                                ? "Loading..."
                                : "Apply"
                            enabled: !((modelData && (modelData.assetLoading || false)) || false)
                            onClicked: root.requestSuggestionApply(index)
                        }
                    }
                }
            }
        }
    }
}
