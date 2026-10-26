// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "../components" as Components

Item {
    id: root

    required property var windowRoot
    required property bool viewerOpen
    required property bool useWholeScreenViewerMode
    required property int popupTransitionMs
    required property string titleText
    required property var closeViewer
    required property var toggleInfoVisible
    required property var switchComparisonImage
    required property var openContextMenu
    // Actions whose shortcuts also work in the viewer.
    property var viewerActions: []
    // The app's errors and palette; the viewer shows the newest error, as
    // it hides the main window's.
    property var errorController: null
    property var uiPalette: null

    property alias popupHost: albumArtPopupHost
    property alias windowHost: albumArtWindowHost
    readonly property real popupWidth: albumArtViewer.width
    readonly property real popupHeight: albumArtViewer.height
    readonly property real wholeScreenWidth: albumArtFullscreenWindow.width
    readonly property real wholeScreenHeight: albumArtFullscreenWindow.height
    readonly property bool wholeScreenVisible: albumArtFullscreenWindow.visibility === Window.FullScreen

    onViewerOpenChanged: Qt.callLater(root.syncPresentation)
    onUseWholeScreenViewerModeChanged: {
        if (root.viewerOpen) {
            Qt.callLater(root.syncPresentation)
        }
    }

    function syncPresentation() {
        if (root.viewerOpen && !root.useWholeScreenViewerMode) {
            if (!albumArtViewer.visible) {
                albumArtViewer.open()
            }
        } else if (albumArtViewer.visible) {
            albumArtViewer.close()
        }
        if (root.viewerOpen && root.useWholeScreenViewerMode) {
            albumArtFullscreenWindow.requestActivate()
            root.focusFullscreen()
        }
    }

    function focusFullscreen() {
        if (root.wholeScreenVisible) {
            albumArtFullscreenFocusSink.forceActiveFocus()
        }
    }

    Component.onCompleted: Qt.callLater(root.syncPresentation)

    Popup {
        id: albumArtViewer
        objectName: "albumArtViewerPopup"
        parent: Overlay.overlay
        x: 0
        y: 0
        width: root.windowRoot.width
        height: root.windowRoot.height
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape
        visible: root.viewerOpen && !root.useWholeScreenViewerMode
        enter: Components.PopupTransition { duration: root.popupTransitionMs }
        exit: Components.PopupTransition { duration: root.popupTransitionMs }

        Components.ActionShortcuts {
            actions: root.viewerActions
            active: albumArtViewer.visible
        }

        Shortcut {
            sequence: "I"
            context: Qt.WindowShortcut
            enabled: albumArtViewer.visible
            onActivated: root.toggleInfoVisible()
        }

        Shortcut {
            sequences: ["Menu", "Shift+F10"]
            context: Qt.WindowShortcut
            enabled: albumArtViewer.visible
            onActivated: root.openContextMenu()
        }

        Shortcut {
            sequence: "Left"
            context: Qt.WindowShortcut
            enabled: albumArtViewer.visible
            onActivated: root.switchComparisonImage()
        }

        Shortcut {
            sequence: "Right"
            context: Qt.WindowShortcut
            enabled: albumArtViewer.visible
            onActivated: root.switchComparisonImage()
        }

        onClosed: {
            if (root.viewerOpen && !root.useWholeScreenViewerMode) {
                root.closeViewer()
            }
        }

        background: Rectangle {
            color: "#000000"
            opacity: 0.87
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onClicked: root.closeViewer()
        }

        Components.ViewerCloseButton {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            fillColor: Qt.rgba(1, 1, 1, 0.16)
            borderColor: Qt.rgba(1, 1, 1, 0.52)
            onClicked: root.closeViewer()
        }

        Item {
            id: albumArtPopupHost
            anchors.fill: parent
        }

        Components.ErrorBanner {
            objectName: "albumArtPopupErrorBanner"
            z: 30
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: 24
            width: Math.min(parent.width - 48, 720)
            errorController: root.errorController
            uiPalette: root.uiPalette
            historyAvailable: false
            active: root.viewerOpen && !root.useWholeScreenViewerMode
        }
    }

    Window {
        id: albumArtFullscreenWindow
        objectName: "albumArtFullscreenWindow"
        screen: root.windowRoot.screen
        transientParent: root.windowRoot
        modality: Qt.ApplicationModal
        flags: Qt.Window | Qt.FramelessWindowHint
        visibility: root.viewerOpen && root.useWholeScreenViewerMode
            ? Window.FullScreen
            : Window.Hidden
        color: "#000000"
        title: root.titleText

        onVisibilityChanged: function() {
            if (albumArtFullscreenWindow.visibility === Window.FullScreen) {
                requestActivate()
                root.focusFullscreen()
            }
        }

        onClosing: function(close) {
            if (root.viewerOpen && root.useWholeScreenViewerMode) {
                root.closeViewer()
            }
        }

        FocusScope {
            id: albumArtFullscreenFocusSink
            anchors.fill: parent
            focus: albumArtFullscreenWindow.visibility === Window.FullScreen
            // Above the viewer, and inside the scope that handles the
            // viewer's keys, so they work while the error's buttons have focus.
            z: 30

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Escape) {
                    event.accepted = true
                    root.closeViewer()
                } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                    event.accepted = true
                    root.switchComparisonImage()
                } else if (event.key === Qt.Key_Menu
                        || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                    event.accepted = true
                    root.openContextMenu()
                }
            }

            Components.ErrorBanner {
                objectName: "albumArtFullscreenErrorBanner"
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottomMargin: 24
                width: Math.min(parent.width - 48, 720)
                errorController: root.errorController
                uiPalette: root.uiPalette
                historyAvailable: false
                active: root.viewerOpen && root.useWholeScreenViewerMode
            }
        }

        Shortcut {
            sequence: "I"
            context: Qt.WindowShortcut
            enabled: albumArtFullscreenWindow.visibility === Window.FullScreen
            onActivated: root.toggleInfoVisible()
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: root.focusFullscreen()
            onClicked: root.closeViewer()
        }

        Components.ViewerCloseButton {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            fillColor: Qt.rgba(1, 1, 1, 0.16)
            borderColor: Qt.rgba(1, 1, 1, 0.52)
            onClicked: root.closeViewer()
        }

        Item {
            anchors.fill: parent

            Item {
                id: albumArtWindowHost
                anchors.fill: parent
            }
        }
    }
}
