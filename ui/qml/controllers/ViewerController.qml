import QtQuick 2.15
import "../logic/PathUtils.js" as PathUtils

QtObject {
    id: root

    required property var uiBridge
    property bool useWholeScreenViewerMode: false

    property int albumArtViewResetToken: 0
    property bool albumArtViewerOpen: false
    property bool albumArtInfoVisible: false
    property var albumArtViewerFileInfo: ({})
    property string albumArtViewerInfoSource: ""
    property string albumArtViewerSource: ""
    property bool albumArtViewerShowsCurrentTrack: true
    property bool spectrogramViewerOpen: false

    function closeAlbumArtViewer() {
        root.albumArtViewerOpen = false
    }

    function closeSpectrogramViewer() {
        root.spectrogramViewerOpen = false
    }

    function openSpectrogramViewer() {
        root.spectrogramViewerOpen = true
    }

    function refreshAlbumArtFileInfo() {
        const infoSource = root.albumArtViewerInfoSource || ""
        root.albumArtViewerFileInfo = infoSource.length > 0
            ? (root.uiBridge.imageFileDetails(infoSource) || ({}))
            : ({})
    }

    function albumArtInfoOverlayText() {
        const info = root.albumArtViewerFileInfo || ({})
        const lines = []

        if ((info.fileName || "").length > 0) {
            lines.push("File: " + info.fileName)
        }
        if ((info.resolutionText || "").length > 0) {
            lines.push("Resolution: " + info.resolutionText)
        }
        if ((info.fileSizeText || "").length > 0) {
            lines.push("Size: " + info.fileSizeText)
        }
        if ((info.fileType || "").length > 0) {
            lines.push("Type: " + info.fileType)
        }
        if ((info.mimeType || "").length > 0) {
            lines.push("MIME: " + info.mimeType)
        }
        if ((info.path || "").length > 0) {
            lines.push("Path: " + info.path)
        }

        return lines.join("\n")
    }

    function openAlbumArtViewer() {
        if (!root.uiBridge.currentTrackCoverPath || root.uiBridge.currentTrackCoverPath.length === 0) {
            return
        }
        root.albumArtViewerSource = root.uiBridge.currentTrackCoverPath || ""
        root.albumArtViewerInfoSource = PathUtils.pathFromAnyUrl(root.albumArtViewerSource)
        root.albumArtViewerShowsCurrentTrack = true
        root.albumArtInfoVisible = false
        root.albumArtViewResetToken += 1
        root.albumArtViewerFileInfo = ({})
        root.albumArtViewerOpen = true
    }

    function openAlbumArtViewerForSuggestion(rowMap) {
        const previewSource = (rowMap && (rowMap.normalizedUrl || rowMap.previewSource || "")) || ""
        if (previewSource.length === 0) {
            return
        }
        root.albumArtViewerSource = previewSource
        root.albumArtViewerInfoSource = (rowMap && (rowMap.normalizedPath || ""))
            || PathUtils.pathFromAnyUrl(previewSource)
        root.albumArtViewerShowsCurrentTrack = false
        root.albumArtInfoVisible = true
        root.albumArtViewResetToken += 1
        root.refreshAlbumArtFileInfo()
        root.albumArtViewerOpen = true
    }

    function toggleAlbumArtInfoVisible(focusFullscreen) {
        if (!root.albumArtViewerOpen) {
            return
        }
        if (!root.albumArtInfoVisible) {
            root.refreshAlbumArtFileInfo()
        }
        root.albumArtInfoVisible = !root.albumArtInfoVisible
        if (focusFullscreen) {
            focusFullscreen()
        }
    }

    function currentTrackItunesArtworkDisabledReason() {
        if ((root.uiBridge.currentTrackPath || "").trim().length === 0) {
            return "No active track."
        }
        if ((root.uiBridge.currentTrackAlbum || "").trim().length === 0) {
            return "Album metadata is missing."
        }
        if ((root.uiBridge.currentTrackArtist || "").trim().length === 0) {
            return "Artist metadata is missing."
        }
        return ""
    }

    function openItunesArtworkDialog(dialog, wholeScreenParent, overlayParent) {
        const targetParent = root.albumArtViewerOpen && root.useWholeScreenViewerMode
            ? wholeScreenParent
            : overlayParent
        Qt.callLater(function() {
            dialog.parent = targetParent
            root.uiBridge.searchCurrentTrackArtworkSuggestions()
            dialog.open()
        })
    }

    function handleSnapshotChanged() {
        if (root.albumArtViewerOpen
                && root.albumArtViewerShowsCurrentTrack
                && root.albumArtViewerSource !== (root.uiBridge.currentTrackCoverPath || "")) {
            root.albumArtViewerSource = root.uiBridge.currentTrackCoverPath || ""
            root.albumArtViewerInfoSource = PathUtils.pathFromAnyUrl(root.albumArtViewerSource)
            if (root.albumArtInfoVisible) {
                root.refreshAlbumArtFileInfo()
            } else {
                root.albumArtViewerFileInfo = ({})
            }
        } else if (root.albumArtViewerOpen
                && root.albumArtViewerInfoSource
                    !== PathUtils.pathFromAnyUrl(root.albumArtViewerSource || "")) {
            root.albumArtViewerInfoSource = PathUtils.pathFromAnyUrl(root.albumArtViewerSource || "")
            if (root.albumArtInfoVisible) {
                root.refreshAlbumArtFileInfo()
            } else {
                root.albumArtViewerFileInfo = ({})
            }
        }
    }
}
