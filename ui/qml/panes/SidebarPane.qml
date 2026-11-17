// SPDX-License-Identifier: GPL-3.0-or-later

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
    // The least width, which follows the font (WindowLayoutController).
    property real minimumWidth: 250
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    required property var stepScrollView
    required property var addLibraryFolderAction
    // The tracks the playback failed to read, which the tree marks.
    property var errorController: null

    // The library rows the now-playing panel leaves room for, under the
    // library's header: under them, it takes its narrow layout.
    readonly property int leastLibraryRows: 6

    // Content: the view colour, from the now-playing cover to the bottom
    // of the library.
    color: root.uiPalette.view
    SplitView.preferredWidth: root.splitPreferredWidth
    SplitView.minimumWidth: root.minimumWidth

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Components.NowPlayingPanel {
            id: nowPlaying
            objectName: "nowPlayingPanel"
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            availableHeight: root.height
            libraryReserve: (1 + root.leastLibraryRows) * root.uiPalette.rowHeight
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            replaceFromItunesAction: root.replaceFromItunesAction
            currentTrackItunesArtworkDisabledReason: root.currentTrackItunesArtworkDisabledReason
            openAlbumArtViewer: root.openAlbumArtViewer
        }

        LibraryPane {
            objectName: "libraryPane"
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: root.controller
            uiBridge: root.uiBridge
            libraryModel: root.libraryModel
            uiPalette: root.uiPalette
            snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
            snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
            stepScrollView: root.stepScrollView
            addLibraryFolderAction: root.addLibraryFolderAction
            narrow: nowPlaying.narrow
            errorController: root.errorController
        }
    }
}
