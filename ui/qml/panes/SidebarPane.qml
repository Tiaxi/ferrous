import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../components" as Components
import "."

Rectangle {
    id: root

    required property var controller
    required property var uiBridge
    required property var libraryModel
    required property var uiPalette
    required property real splitPreferredWidth
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer
    required property var queueTrackNumberText
    required property int popupTransitionMs
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property var stepScrollView
    required property var playAllLibraryTracksAction
    required property var appendAllLibraryTracksAction

    color: root.uiPalette.uiPaneColor
    SplitView.preferredWidth: root.splitPreferredWidth
    SplitView.minimumWidth: 250

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Components.AlbumArtTile {
            Layout.fillWidth: true
            Layout.preferredHeight: width
            uiBridge: root.uiBridge
            replaceFromItunesAction: root.replaceFromItunesAction
            currentTrackItunesArtworkDisabledReason: root.currentTrackItunesArtworkDisabledReason
            openAlbumArtViewer: root.openAlbumArtViewer
        }

        LibraryPane {
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: root.controller
            uiBridge: root.uiBridge
            libraryModel: root.libraryModel
            uiPalette: root.uiPalette
            queueTrackNumberText: root.queueTrackNumberText
            snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
            snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
            popupTransitionMs: root.popupTransitionMs
            stepScrollView: root.stepScrollView
            playAllLibraryTracksAction: root.playAllLibraryTracksAction
            appendAllLibraryTracksAction: root.appendAllLibraryTracksAction
        }
    }
}
