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
    // The app's errors; the viewer shows the newest error, as it hides the
    // main window's status bar.
    property var errorController: null

    property alias popupHost: albumArtPopupHost
    // The window of the viewer over the whole screen, and the item in it
    // that holds the album art; null until the viewer first opens there.
    readonly property Window fullscreenWindow: albumArtFullscreenWindowLoader.item
    readonly property Item windowHost: root.fullscreenWindow ? root.fullscreenWindow.host : null
    readonly property real popupWidth: albumArtViewer.width
    readonly property real popupHeight: albumArtViewer.height
    readonly property real wholeScreenWidth: root.fullscreenWindow ? root.fullscreenWindow.width : 0
    readonly property real wholeScreenHeight: root.fullscreenWindow ? root.fullscreenWindow.height : 0
    readonly property bool wholeScreenVisible: root.fullscreenWindow !== null
        && root.fullscreenWindow.visibility === Window.FullScreen
    readonly property bool wholeScreenOpen: root.viewerOpen && root.useWholeScreenViewerMode

    // Made the first time the viewer opens over the whole screen and kept,
    // hidden while the viewer is closed, as the visualization viewer's is.
    onWholeScreenOpenChanged: {
        if (root.wholeScreenOpen) {
            albumArtFullscreenWindowLoader.active = true
        }
    }

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
        if (root.wholeScreenOpen && root.fullscreenWindow) {
            root.fullscreenWindow.requestActivate()
            root.focusFullscreen()
        }
    }

    function focusFullscreen() {
        if (root.wholeScreenVisible) {
            root.fullscreenWindow.focusSink.forceActiveFocus()
        }
    }

    Component.onCompleted: {
        if (root.wholeScreenOpen) {
            albumArtFullscreenWindowLoader.active = true
        }
        Qt.callLater(root.syncPresentation)
    }

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
            active: root.viewerOpen && !root.useWholeScreenViewerMode
        }
    }

    Loader {
        id: albumArtFullscreenWindowLoader
        objectName: "albumArtFullscreenWindowLoader"
        active: false

        sourceComponent: Component {
            Window {
                id: albumArtFullscreenWindow
                objectName: "albumArtFullscreenWindow"
                readonly property Item host: albumArtWindowHost
                readonly property Item focusSink: albumArtFullscreenFocusSink
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
    }
}
