// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "../components" as Components

Item {
    id: root

    required property var windowRoot
    required property var uiPalette
    required property bool viewerOpen
    required property bool useWholeScreenViewerMode
    required property int popupTransitionMs
    required property string titleText
    required property var closeViewer
    property int visualizationMode: 0
    property var setVisualizationMode: function(mode) {}
    property var displaySleepInhibitor: null
    property bool displaySleepInhibitionEnabled: true
    property bool playbackPlaying: false
    // Actions whose shortcuts also work in the viewer.
    property var viewerActions: []
    // The app's errors. The viewer covers the main window's status bar, so
    // new ones, such as a screen it cannot keep awake, show as a button
    // that opens Recent Issues.
    property var errorController: null
    // The playing track and the playback clock's position text, for the
    // title strip.
    property var uiBridge: null
    property string positionText: "00:00"
    // The view's zoom, 1 at the default, and what resets it: the same as
    // View › Reset Visualization View.
    property real zoomLevel: 1.0
    property var resetView: null
    // How far the corner controls keep from the right edge, clear of the
    // level meter's column.
    property real cornerRightMargin: 4

    property alias popupHost: spectrogramPopupHost
    // The window of the viewer over the whole screen, and the item in it
    // that holds the visualization; null until the viewer first opens there.
    readonly property Window fullscreenWindow: spectrogramFullscreenWindowLoader.item
    readonly property Item windowHost: root.fullscreenWindow ? root.fullscreenWindow.host : null
    property alias fullscreenControlsHideDelay: fullscreenControlsAutoHide.hideDelay
    // What the visualization keeps clear of in its top right corner: in the
    // main window, the viewer's controls, with the plot layer's mode switch
    // 4 px left of them; over the whole screen nothing, as the controls are
    // in the title strip above the view. It does not change as they
    // auto-hide.
    readonly property size reservedCorner: root.useWholeScreenViewerMode
        ? Qt.size(0, 0)
        : Qt.size(popupControls.width + 4,
                  popupControls.anchors.topMargin + popupControls.height)
    readonly property bool fullscreenControlsVisible: fullscreenControlsAutoHide.controlsVisible
    readonly property bool fullscreenDisplayActive:
        root.viewerOpen
        && root.useWholeScreenViewerMode
        && root.displaySleepInhibitionEnabled
        && root.fullscreenWindow !== null
        && root.fullscreenWindow.visibility === Window.FullScreen
    // A paused or stopped fullscreen view lets the screen blank as usual.
    readonly property bool displaySleepInhibited:
        root.fullscreenDisplayActive && root.playbackPlaying

    function syncDisplaySleepInhibition() {
        if (root.displaySleepInhibitor
                && root.displaySleepInhibitor.setInhibited) {
            root.displaySleepInhibitor.setInhibited(root.displaySleepInhibited)
        }
    }

    function noteFullscreenPointerActivity(x, y) {
        fullscreenControlsAutoHide.pointerMoved(x, y)
    }

    // The item with keyboard focus in the viewer's window.
    readonly property Item focusedItem: root.wholeScreenOpen
        ? (root.fullscreenWindow ? root.fullscreenWindow.activeFocusItem : null)
        : (root.windowRoot ? root.windowRoot.activeFocusItem : null)

    Components.FullscreenControlsAutoHide {
        id: fullscreenControlsAutoHide
        objectName: "spectrogramFullscreenControlsAutoHide"
        active: root.viewerOpen
        // A control with keyboard focus stays shown.
        held: root.focusedItem !== null && root.focusedItem.visualFocus === true
    }

    readonly property bool wholeScreenOpen: root.viewerOpen && root.useWholeScreenViewerMode
    // The window is made the first time the viewer opens over the whole
    // screen, not at startup, and kept from then on: it only hides when
    // the viewer closes. Qt keeps the graphics and the scene graph of a
    // hidden window (persistentGraphics and persistentSceneGraph, true by
    // default), so opening it again sets none of them up again.
    onWholeScreenOpenChanged: {
        if (root.wholeScreenOpen) {
            spectrogramFullscreenWindowLoader.active = true
        }
    }

    onViewerOpenChanged: Qt.callLater(root.syncPresentation)
    onDisplaySleepInhibitedChanged: root.syncDisplaySleepInhibition()
    onDisplaySleepInhibitorChanged: root.syncDisplaySleepInhibition()
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
        if (root.wholeScreenOpen && root.fullscreenWindow) {
            root.fullscreenWindow.requestActivate()
        }
    }

    Component.onCompleted: {
        if (root.wholeScreenOpen) {
            spectrogramFullscreenWindowLoader.active = true
        }
        Qt.callLater(root.syncPresentation)
        root.syncDisplaySleepInhibition()
    }
    Component.onDestruction: {
        if (root.displaySleepInhibitor
                && root.displaySleepInhibitor.setInhibited) {
            root.displaySleepInhibitor.setInhibited(false)
        }
    }

    Popup {
        id: spectrogramViewer
        objectName: "spectrogramViewerPopup"
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
            color: root.uiPalette.plotColors(root.visualizationMode).plotBg
        }

        Components.ActionShortcuts {
            actions: root.viewerActions
            active: spectrogramViewer.visible
        }

        // The window's title names the viewer, so it has no title strip:
        // its controls are on a plate in the top right corner.
        FocusScope {
            id: spectrogramPopupFocusScope
            anchors.fill: parent
            focus: true

            // A key brings the controls back, so Tab reaches them.
            Keys.onPressed: function(event) {
                fullscreenControlsAutoHide.wake()
                event.accepted = false
            }

            Rectangle {
                objectName: "spectrogramPopupFloor"
                anchors.fill: parent
                color: root.uiPalette.plotColors(root.visualizationMode).plotBg

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onPressed: spectrogramPopupFocusScope.forceActiveFocus()
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

            Components.ViewerCornerControls {
                id: popupControls
                objectName: "spectrogramPopupControls"
                z: 20
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 4
                anchors.rightMargin: root.cornerRightMargin
                plotColors: root.uiPalette.plotColors(root.visualizationMode)
                uiPalette: root.uiPalette
                closeButtonName: "spectrogramPopupCloseButton"
                controlsVisible: root.fullscreenControlsVisible
                pointerActivity: root.noteFullscreenPointerActivity
                zoomLevel: root.zoomLevel
                resetView: root.resetView
                errorController: root.errorController
                closeViewer: root.closeViewer
            }
        }
    }

    Loader {
        id: spectrogramFullscreenWindowLoader
        objectName: "spectrogramFullscreenWindowLoader"
        active: false

        sourceComponent: Component {
            Window {
                id: spectrogramFullscreenWindow
                objectName: "spectrogramFullscreenWindow"
                readonly property Item host: spectrogramWindowHost
                readonly property Item strip: spectrogramStrip
                screen: root.windowRoot.screen
                transientParent: root.windowRoot
                modality: Qt.ApplicationModal
                flags: Qt.Window | Qt.FramelessWindowHint
                visibility: root.viewerOpen && root.useWholeScreenViewerMode
                    ? Window.FullScreen
                    : Window.Hidden
                color: root.uiPalette.plotColors(root.visualizationMode).plotBg
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

                // The view, under the title strip. The strip never hides, so
                // the view keeps its size as the controls auto-hide.
                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: spectrogramStrip.height
                    color: root.uiPalette.plotColors(root.visualizationMode).plotBg

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

                FocusScope {
                    id: spectrogramFullscreenFocusSink
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: spectrogramStrip.height
                    focus: spectrogramFullscreenWindow.visibility === Window.FullScreen

                    // Escape closes the viewer, also while one of the strip's
                    // buttons has focus; any key brings the controls back.
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Escape) {
                            event.accepted = true
                            root.closeViewer()
                            return
                        }
                        fullscreenControlsAutoHide.wake()
                    }

                    Components.ViewerStrip {
                        id: spectrogramStrip
                        objectName: "spectrogramFullscreenStrip"
                        anchors.fill: parent
                        uiPalette: root.uiPalette
                        uiBridge: root.uiBridge
                        positionText: root.positionText
                        closeButtonName: "spectrogramFullscreenCloseButton"
                        controlsVisible: root.fullscreenControlsVisible
                        pointerActivity: root.noteFullscreenPointerActivity
                        zoomLevel: root.zoomLevel
                        resetView: root.resetView
                        errorController: root.errorController
                        closeViewer: root.closeViewer
                    }
                }
            }
        }
    }
}
