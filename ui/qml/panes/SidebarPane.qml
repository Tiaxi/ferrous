import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../components" as Components
import "."

Rectangle {
    id: root

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
    required property string pendingLibraryExpandFitKey
    required property var applyPendingLibraryExpansionFit
    required property var stepScrollView
    required property var handleLibraryKeyPress
    required property var isLibrarySelectionKeySelected
    required property var toggleLibraryNode
    required property var handleLibraryRowSelection
    required property var rowsForLibraryAction
    required property var playLibraryRows
    required property var appendLibraryRows
    required property var isActionableLibraryRow
    required property var canOpenTagEditorForLibrary
    required property var openTagEditorForLibrary
    required property var isLibraryTreeLoading
    required property var playAllLibraryTracksAction
    required property var appendAllLibraryTracksAction

    signal viewReady(var view)

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
            uiBridge: root.uiBridge
            libraryModel: root.libraryModel
            uiPalette: root.uiPalette
            queueTrackNumberText: root.queueTrackNumberText
            snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
            snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
            popupTransitionMs: root.popupTransitionMs
            pendingLibraryExpandFitKey: root.pendingLibraryExpandFitKey
            applyPendingLibraryExpansionFit: root.applyPendingLibraryExpansionFit
            stepScrollView: root.stepScrollView
            handleLibraryKeyPress: root.handleLibraryKeyPress
            isLibrarySelectionKeySelected: root.isLibrarySelectionKeySelected
            toggleLibraryNode: root.toggleLibraryNode
            handleLibraryRowSelection: root.handleLibraryRowSelection
            rowsForLibraryAction: root.rowsForLibraryAction
            playLibraryRows: root.playLibraryRows
            appendLibraryRows: root.appendLibraryRows
            isActionableLibraryRow: root.isActionableLibraryRow
            canOpenTagEditorForLibrary: root.canOpenTagEditorForLibrary
            openTagEditorForLibrary: root.openTagEditorForLibrary
            isLibraryTreeLoading: root.isLibraryTreeLoading
            playAllLibraryTracksAction: root.playAllLibraryTracksAction
            appendAllLibraryTracksAction: root.appendAllLibraryTracksAction
            onViewReady: function(view) {
                root.viewReady(view)
            }
        }
    }
}
