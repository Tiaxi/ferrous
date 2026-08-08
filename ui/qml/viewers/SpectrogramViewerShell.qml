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
    property int visualizationMode: 0
    property var setVisualizationMode: function(mode) {}

    property alias popupHost: spectrogramPopupHost
    property alias windowHost: spectrogramWindowHost
    property alias fullscreenControlsHideDelay: fullscreenControlsAutoHide.hideDelay
    readonly property bool fullscreenControlsVisible: fullscreenControlsAutoHide.controlsVisible

    function noteFullscreenPointerActivity(x, y) {
        fullscreenControlsAutoHide.pointerMoved(x, y)
    }

    Components.FullscreenControlsAutoHide {
        id: fullscreenControlsAutoHide
        objectName: "spectrogramFullscreenControlsAutoHide"
        active: root.viewerOpen
    }

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
            objectName: "spectrogramPopupCloseButton"
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            visible: root.fullscreenControlsVisible
            fillColor: Qt.rgba(0, 0, 0, 0.45)
            borderColor: Qt.rgba(1, 1, 1, 0.24)
            onClicked: root.closeViewer()

            HoverHandler {
                onPointChanged: root.noteFullscreenPointerActivity(
                    point.scenePosition.x, point.scenePosition.y)
            }
        }

        Components.VisualizationModeSwitch {
            z: 20
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 12
            controlsVisible: root.fullscreenControlsVisible
            selectedMode: root.visualizationMode
            setMode: root.setVisualizationMode
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
            objectName: "spectrogramFullscreenCloseButton"
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            visible: root.fullscreenControlsVisible
            fillColor: Qt.rgba(0, 0, 0, 0.45)
            borderColor: Qt.rgba(1, 1, 1, 0.24)
            onClicked: root.closeViewer()

            HoverHandler {
                onPointChanged: root.noteFullscreenPointerActivity(
                    point.scenePosition.x, point.scenePosition.y)
            }
        }


        Components.VisualizationModeSwitch {
            z: 20
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 12
            controlsVisible: root.fullscreenControlsVisible
            selectedMode: root.visualizationMode
            setMode: root.setVisualizationMode
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
