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

    property alias popupHost: spectrogramPopupHost
    property alias windowHost: spectrogramWindowHost

    onViewerOpenChanged: Qt.callLater(root.syncPresentation)
    onUseWholeScreenViewerModeChanged: {
        if (root.viewerOpen) {
            Qt.callLater(root.syncPresentation)
        }
    }

    function syncPresentation() {
        if (root.viewerOpen && !root.useWholeScreenViewerMode) {
            if (!spectrogramViewer.visible) {
                spectrogramViewer.open()
            }
        } else if (spectrogramViewer.visible) {
            spectrogramViewer.close()
        }
        if (root.viewerOpen && root.useWholeScreenViewerMode) {
            spectrogramFullscreenWindow.requestActivate()
        }
    }

    Component.onCompleted: Qt.callLater(root.syncPresentation)

    Popup {
        id: spectrogramViewer
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
            fillColor: Qt.rgba(0, 0, 0, 0.45)
            borderColor: Qt.rgba(1, 1, 1, 0.24)
            onClicked: root.closeViewer()
        }

        Rectangle {
            anchors.fill: parent
            color: "#0b0b0f"
            border.color: Qt.rgba(1, 1, 1, 0.12)

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onDoubleClicked: function(mouse) {
                    if (mouse.button === Qt.LeftButton) {
                        root.closeViewer()
                    }
                }
            }

            Item {
                id: spectrogramPopupHost
                anchors.fill: parent
            }
        }
    }

    Window {
        id: spectrogramFullscreenWindow
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
            if (spectrogramFullscreenWindow.visibility === Window.FullScreen) {
                requestActivate()
                spectrogramFullscreenFocusSink.forceActiveFocus()
            }
        }

        onClosing: function(close) {
            if (root.viewerOpen && root.useWholeScreenViewerMode) {
                root.closeViewer()
            }
        }

        FocusScope {
            id: spectrogramFullscreenFocusSink
            anchors.fill: parent
            focus: spectrogramFullscreenWindow.visibility === Window.FullScreen

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Escape) {
                    event.accepted = true
                    root.closeViewer()
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: spectrogramFullscreenFocusSink.forceActiveFocus()
            onClicked: root.closeViewer()
        }

        Components.ViewerCloseButton {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            fillColor: Qt.rgba(0, 0, 0, 0.45)
            borderColor: Qt.rgba(1, 1, 1, 0.24)
            onClicked: root.closeViewer()
        }

        Rectangle {
            anchors.fill: parent
            color: "#0b0b0f"

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: spectrogramFullscreenFocusSink.forceActiveFocus()
                onDoubleClicked: function(mouse) {
                    if (mouse.button === Qt.LeftButton) {
                        root.closeViewer()
                    }
                }
            }

            Item {
                id: spectrogramWindowHost
                anchors.fill: parent
            }
        }
    }
}
