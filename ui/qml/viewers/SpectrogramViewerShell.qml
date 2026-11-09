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
    // Whether the title strip shows over the whole screen when the pointer
    // moves: the Preferences setting.
    property bool stripEnabled: true

    property alias popupHost: spectrogramPopupHost
    // The window of the viewer over the whole screen, and the item in it
    // that holds the visualization; null until the viewer first opens there.
    readonly property Window fullscreenWindow: spectrogramFullscreenWindowLoader.item
    readonly property Item windowHost: root.fullscreenWindow ? root.fullscreenWindow.host : null
    property alias fullscreenControlsHideDelay: fullscreenControlsAutoHide.hideDelay
    // What the visualization keeps clear of in its top right corner: in the
    // main window, the viewer's controls, with the plot layer's mode switch
    // 4 px left of them; over the whole screen nothing, as the controls are
    // in the title strip. It does not change as they auto-hide.
    readonly property size reservedCorner: root.useWholeScreenViewerMode
        ? Qt.size(0, 0)
        : Qt.size(popupControls.width + 4,
                  popupControls.anchors.topMargin + popupControls.height)
    // How far the visualization's controls, such as its mode switch and the
    // first lane's Mute and Solo, keep below its top edge: over the whole
    // screen, the title strip's height while it can show. They show only
    // with the viewer's controls, so with the strip over the top edge; they
    // keep there while it is hidden, rather than move each time it shows.
    // What only reads, such as the time scale, stays on the edge and shows
    // while it is hidden. With the strip turned off, they are on the edge.
    readonly property real controlsTopInset: root.wholeScreenOpen && root.fullscreenWindow
            && root.stripEnabled
        ? root.fullscreenWindow.strip.implicitHeight : 0
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

    // The focus scope that holds the whole viewer, its view and controls,
    // and the item in it that has focus while the view has it.
    readonly property Item viewerScope: root.wholeScreenOpen
        ? (root.fullscreenWindow ? root.fullscreenWindow.focusScope : null)
        : spectrogramPopupFocusScope
    readonly property Item viewFocus: root.wholeScreenOpen
        ? (root.fullscreenWindow ? root.fullscreenWindow.viewFocus : null)
        : spectrogramPopupViewFocus

    // Whether `item` is inside `scope`, and not the scope itself.
    function isInside(item, scope) {
        if (item === null || scope === null || item === scope) {
            return false
        }
        for (let parentItem = item.parent; parentItem; parentItem = parentItem.parent) {
            if (parentItem === scope) {
                return true
            }
        }
        return false
    }

    Components.FullscreenControlsAutoHide {
        id: fullscreenControlsAutoHide
        objectName: "spectrogramFullscreenControlsAutoHide"
        active: root.viewerOpen
        // The controls stay while one of them has focus, however it got it:
        // the keyboard, a click, or assistive technology.
        held: root.focusedItem !== root.viewFocus
            && root.isInside(root.focusedItem, root.viewerScope)
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
        // The style's own transitions.
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

            // Takes focus from the controls when the view is pressed.
            Item {
                id: spectrogramPopupViewFocus
                objectName: "spectrogramPopupViewFocus"
            }

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

                // A press anywhere on the view takes the focus from the
                // controls, so they can hide again, then goes on to the view.
                MouseArea {
                    anchors.fill: parent
                    z: 1
                    acceptedButtons: Qt.AllButtons
                    onPressed: function(mouse) {
                        spectrogramPopupViewFocus.forceActiveFocus(Qt.MouseFocusReason)
                        mouse.accepted = false
                    }
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
                readonly property Item focusScope: spectrogramFullscreenFocusSink
                readonly property Item viewFocus: spectrogramFullscreenViewFocus
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

                // The whole viewer: its keys reach every control in it, in
                // the strip and in the view alike.
                FocusScope {
                    id: spectrogramFullscreenFocusSink
                    objectName: "spectrogramFullscreenFocusScope"
                    anchors.fill: parent
                    focus: spectrogramFullscreenWindow.visibility === Window.FullScreen

                    // Takes focus from the controls when the view is pressed.
                    Item {
                        id: spectrogramFullscreenViewFocus
                        objectName: "spectrogramFullscreenViewFocus"
                    }

                    // Escape closes the viewer, whichever control has focus;
                    // any key brings the controls back.
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Escape) {
                            event.accepted = true
                            root.closeViewer()
                            return
                        }
                        fullscreenControlsAutoHide.wake()
                    }

                    // The view, over the whole window: the title strip is
                    // drawn over its top edge.
                    Rectangle {
                        anchors.fill: parent
                        color: root.uiPalette.plotColors(root.visualizationMode).plotBg

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
                            id: spectrogramWindowHost
                            anchors.fill: parent
                        }

                        // A press anywhere on the view takes the focus from
                        // the controls, so they can hide again, then goes on
                        // to the view.
                        MouseArea {
                            anchors.fill: parent
                            z: 1
                            acceptedButtons: Qt.AllButtons
                            onPressed: function(mouse) {
                                spectrogramFullscreenViewFocus.forceActiveFocus(Qt.MouseFocusReason)
                                mouse.accepted = false
                            }
                        }
                    }

                    Components.ViewerStrip {
                        id: spectrogramStrip
                        objectName: "spectrogramFullscreenStrip"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        z: 1
                        uiPalette: root.uiPalette
                        plotColors: root.uiPalette.plotColors(root.visualizationMode)
                        uiBridge: root.uiBridge
                        positionText: root.positionText
                        closeButtonName: "spectrogramFullscreenCloseButton"
                        controlsVisible: root.fullscreenControlsVisible
                        available: root.stripEnabled
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
