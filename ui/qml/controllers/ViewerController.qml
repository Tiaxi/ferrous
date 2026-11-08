// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
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
    property bool comparisonModeAvailable: false
    property string comparisonCurrentSource: ""
    property string comparisonCurrentInfoSource: ""
    property string comparisonCandidateSource: ""
    property string comparisonCandidateInfoSource: ""
    property string comparisonLabel: ""
    property bool spectrogramViewerOpen: false
    // Set while the album art viewer shows a preview from Replace from
    // iTunes: a candidate, or the current art to compare it with. Closing
    // the viewer then goes back to the results, and the viewer shows what
    // it showed before the preview: nothing, or the current art.
    property bool albumArtPreviewActive: false
    property bool albumArtOpenBeforePreview: false
    property bool albumArtInfoVisibleBeforePreview: false
    // The viewer was closed during a preview, by Back to Results, Escape or
    // Close: the results show again.
    signal albumArtPreviewClosed()
    // Where the album art viewer's image is from: "itunes" for a candidate
    // from the iTunes artwork search, "embedded" for art the track's file
    // holds, "folder" for an image beside the music.
    readonly property string albumArtSourceKind: !root.albumArtViewerShowsCurrentTrack
        ? "itunes"
        : ((root.albumArtViewerFileInfo || ({})).embeddedCover === true ? "embedded" : "folder")
    property int visualizationMode: 0

    function setVisualizationMode(mode) {
        root.visualizationMode = Math.max(0, Math.min(1, mode))
    }

    function closeAlbumArtViewer() {
        if (root.albumArtPreviewActive) {
            root.endAlbumArtPreview(false)
            root.albumArtPreviewClosed()
            return
        }
        root.albumArtViewerOpen = false
        root.comparisonModeAvailable = false
        root.comparisonCurrentSource = ""
        root.comparisonCurrentInfoSource = ""
        root.comparisonCandidateSource = ""
        root.comparisonCandidateInfoSource = ""
        root.comparisonLabel = ""
    }

    function closeSpectrogramViewer() {
        root.spectrogramViewerOpen = false
    }

    function openSpectrogramViewer() {
        root.spectrogramViewerOpen = true
    }

    // Opens the visualization viewer in place of the album art viewer, or
    // closes it.
    function toggleSpectrogramViewer() {
        if (root.spectrogramViewerOpen) {
            root.closeSpectrogramViewer()
            return
        }
        if (root.albumArtViewerOpen) {
            root.closeAlbumArtViewer()
        }
        root.openSpectrogramViewer()
    }

    function toggleAlbumArtViewer() {
        if (root.albumArtViewerOpen) {
            root.closeAlbumArtViewer()
            return
        }
        if ((root.uiBridge.currentTrackCoverPath || "").length === 0) {
            return
        }
        if (root.spectrogramViewerOpen) {
            root.closeSpectrogramViewer()
        }
        root.openAlbumArtViewer()
    }

    function refreshAlbumArtFileInfo() {
        const infoSource = root.albumArtViewerInfoSource || ""
        if (infoSource.length <= 0) {
            root.albumArtViewerFileInfo = ({})
            return
        }
        root.uiBridge.requestImageFileDetails(infoSource)
        root.albumArtViewerFileInfo = root.uiBridge.cachedImageFileDetails(infoSource) || ({})
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
        root.comparisonModeAvailable = false
        root.comparisonLabel = ""
        root.albumArtViewerOpen = true
    }

    function beginAlbumArtPreview() {
        if (!root.albumArtPreviewActive) {
            root.albumArtOpenBeforePreview = root.albumArtViewerOpen
            root.albumArtInfoVisibleBeforePreview = root.albumArtInfoVisible
        }
        root.albumArtPreviewActive = true
    }

    // Ends a preview from Replace from iTunes. After Apply, the viewer shows
    // the current art, which becomes the applied cover once the album has
    // it; otherwise it shows what it showed before the preview.
    function endAlbumArtPreview(applied) {
        if (!root.albumArtPreviewActive) {
            return
        }
        root.albumArtPreviewActive = false
        const coverSource = root.uiBridge.currentTrackCoverPath || ""
        if (!applied && (!root.albumArtOpenBeforePreview || coverSource.length === 0)) {
            root.closeAlbumArtViewer()
            return
        }
        root.albumArtViewerSource = coverSource
        root.albumArtViewerInfoSource = PathUtils.pathFromAnyUrl(coverSource)
        root.albumArtViewerShowsCurrentTrack = true
        root.comparisonModeAvailable = false
        root.comparisonCurrentSource = ""
        root.comparisonCurrentInfoSource = ""
        root.comparisonCandidateSource = ""
        root.comparisonCandidateInfoSource = ""
        root.comparisonLabel = ""
        root.albumArtInfoVisible = applied ? root.albumArtInfoVisible : root.albumArtInfoVisibleBeforePreview
        root.albumArtViewResetToken += 1
        if (root.albumArtInfoVisible) {
            root.refreshAlbumArtFileInfo()
        } else {
            root.albumArtViewerFileInfo = ({})
        }
        root.albumArtViewerOpen = true
    }

    // Previews from Replace from iTunes: the current art, or a candidate to
    // compare with it. The current art is the art of the album the search
    // was for, `currentSource`, which the dialog keeps once playback has
    // moved on. Whether the preview opened.
    function previewCurrentArt(currentSource) {
        if ((currentSource || "").length === 0) {
            return false
        }
        root.beginAlbumArtPreview()
        root.showCurrentArt(currentSource)
        return true
    }

    function previewSuggestion(rowMap, currentSource) {
        if (((rowMap && (rowMap.normalizedUrl || rowMap.previewSource || "")) || "").length === 0) {
            return false
        }
        root.beginAlbumArtPreview()
        root.showSuggestion(rowMap, currentSource || "")
        return true
    }

    function openAlbumArtViewerForCurrentArt() {
        root.showCurrentArt(root.uiBridge.currentTrackCoverPath || "")
    }

    function showCurrentArt(currentSource) {
        if (currentSource.length === 0) {
            return
        }
        root.albumArtViewerSource = currentSource
        root.albumArtViewerInfoSource = PathUtils.pathFromAnyUrl(currentSource)
        root.albumArtViewerShowsCurrentTrack = true
        root.comparisonCurrentSource = currentSource
        root.comparisonCurrentInfoSource = root.albumArtViewerInfoSource
        root.comparisonCandidateSource = ""
        root.comparisonCandidateInfoSource = ""
        root.comparisonModeAvailable = false
        root.comparisonLabel = I18n.i18nc("@label", "Current Album Art")
        root.albumArtInfoVisible = true
        root.albumArtViewResetToken += 1
        root.refreshAlbumArtFileInfo()
        root.albumArtViewerOpen = true
    }

    // A candidate, which can be compared with the current art.
    function openAlbumArtViewerForSuggestion(rowMap) {
        root.showSuggestion(rowMap, root.uiBridge.currentTrackCoverPath || "")
    }

    function showSuggestion(rowMap, currentSource) {
        const previewSource = (rowMap && (rowMap.normalizedUrl || rowMap.previewSource || "")) || ""
        if (previewSource.length === 0) {
            return
        }
        root.albumArtViewerSource = previewSource
        root.albumArtViewerInfoSource = (rowMap && (rowMap.normalizedPath || ""))
            || PathUtils.pathFromAnyUrl(previewSource)
        root.albumArtViewerShowsCurrentTrack = false

        root.comparisonCandidateSource = previewSource
        root.comparisonCandidateInfoSource = root.albumArtViewerInfoSource
        root.comparisonCurrentSource = currentSource
        root.comparisonCurrentInfoSource = PathUtils.pathFromAnyUrl(currentSource)
        root.comparisonModeAvailable = currentSource.length > 0
        root.comparisonLabel = I18n.i18nc("@label artwork found on iTunes", "iTunes Candidate")

        root.albumArtInfoVisible = true
        root.albumArtViewResetToken += 1
        root.refreshAlbumArtFileInfo()
        root.albumArtViewerOpen = true
    }

    function switchComparisonImage() {
        if (!root.comparisonModeAvailable) {
            return
        }
        if (root.albumArtViewerShowsCurrentTrack) {
            root.albumArtViewerSource = root.comparisonCandidateSource
            root.albumArtViewerInfoSource = root.comparisonCandidateInfoSource
            root.albumArtViewerShowsCurrentTrack = false
            root.comparisonLabel = I18n.i18nc("@label artwork found on iTunes", "iTunes Candidate")
        } else {
            root.albumArtViewerSource = root.comparisonCurrentSource
            root.albumArtViewerInfoSource = root.comparisonCurrentInfoSource
            root.albumArtViewerShowsCurrentTrack = true
            root.comparisonLabel = I18n.i18nc("@label", "Current Album Art")
        }
        root.albumArtInfoVisible = true
        root.albumArtViewResetToken += 1
        root.refreshAlbumArtFileInfo()
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
            return I18n.i18nc("@info why iTunes artwork cannot be searched", "No active track.")
        }
        if ((root.uiBridge.currentTrackAlbum || "").trim().length === 0) {
            return I18n.i18nc("@info why iTunes artwork cannot be searched", "Album metadata is missing.")
        }
        if ((root.uiBridge.currentTrackArtist || "").trim().length === 0) {
            return I18n.i18nc("@info why iTunes artwork cannot be searched", "Artist metadata is missing.")
        }
        return ""
    }

    function openItunesArtworkDialog(dialog, wholeScreenParent, overlayParent) {
        // From the viewer's menu during a preview: a new search, over what
        // the viewer showed before the preview.
        root.endAlbumArtPreview(false)
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
        // A preview's current art is the searched album's, also once
        // playback has moved on.
        if (root.albumArtViewerOpen
                && !root.albumArtPreviewActive
                && root.albumArtViewerShowsCurrentTrack
                && root.albumArtViewerSource !== (root.uiBridge.currentTrackCoverPath || "")) {
            root.albumArtViewerSource = root.uiBridge.currentTrackCoverPath || ""
            root.albumArtViewerInfoSource = PathUtils.pathFromAnyUrl(root.albumArtViewerSource)
            if (root.comparisonModeAvailable || root.comparisonCurrentSource.length > 0) {
                root.comparisonCurrentSource = root.albumArtViewerSource
                root.comparisonCurrentInfoSource = root.albumArtViewerInfoSource
            }
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

    property var imageDetailsConnection: Connections {
        target: root.uiBridge

        function onImageFileDetailsChanged(path) {
            const infoSource = root.albumArtViewerInfoSource || ""
            if (!root.albumArtViewerOpen || infoSource.length <= 0 || path !== infoSource) {
                return
            }
            root.albumArtViewerFileInfo = root.uiBridge.cachedImageFileDetails(infoSource) || ({})
        }
    }
}
