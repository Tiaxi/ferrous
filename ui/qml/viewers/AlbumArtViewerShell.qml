// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import FerrousUi 1.0
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
    required property var uiPalette
    // Actions whose shortcuts also work in the viewer.
    property var viewerActions: []
    // The app's errors. The viewer covers the main window's status bar, so
    // new ones show as a button that opens Recent Issues.
    property var errorController: null
    // The playing track and the playback clock's position text, for the
    // title strip.
    property var uiBridge: null
    property string positionText: "00:00"
    // Whether the image's details show, for the info button.
    property bool infoVisible: false
    // The art's zoom, 1 when it fits, and what fits it again.
    property real zoomLevel: 1.0
    property var resetView: null

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

    // The info toggle: the tint while the details show, the ring only for
    // keyboard focus.
    component InfoButton: Components.ViewerToolButton {
        uiPalette: root.uiPalette
        // Checked follows the viewer, which the I key toggles too.
        checked: root.infoVisible
        text: I18n.i18nc("@action:button", "Image Details")
        toolTipText: I18n.i18nc("@info:tooltip %1 is the key", "Image Details (%1)", "I")
        icon.name: "documentinfo"
        Accessible.checkable: true
        Accessible.checked: root.infoVisible
        onClicked: root.toggleInfoVisible()
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

        // The art on the view colour.
        background: Rectangle {
            color: root.uiPalette.view
        }

        Item {
            id: albumArtPopupHost
            anchors.fill: parent
        }

        // The window's title names the viewer, so it has no title strip:
        // its controls are on a plate in the top right corner, on the art.
        Components.PlotPlate {
            objectName: "albumArtPopupControls"
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 4
            plotColors: root.uiPalette.waveformPlot
            uiPalette: root.uiPalette
            takesPointer: true

            Components.ViewerControls {
                uiPalette: root.uiPalette
                closeButtonName: "albumArtPopupCloseButton"
                zoomLevel: root.zoomLevel
                resetView: root.resetView
                resetToolTip: I18n.i18nc("@info:tooltip", "Fit the Image to the Window")
                errorController: root.errorController
                closeViewer: root.closeViewer

                InfoButton {
                    objectName: "albumArtPopupInfoButton"
                }
            }
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
                color: root.uiPalette.view
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

                Shortcut {
                    sequence: "I"
                    context: Qt.WindowShortcut
                    enabled: albumArtFullscreenWindow.visibility === Window.FullScreen
                    onActivated: root.toggleInfoVisible()
                }

                // The art, under the title strip.
                Item {
                    anchors.fill: parent
                    anchors.topMargin: albumArtStrip.height

                    Item {
                        id: albumArtWindowHost
                        anchors.fill: parent
                    }
                }

                FocusScope {
                    id: albumArtFullscreenFocusSink
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: albumArtStrip.height
                    focus: albumArtFullscreenWindow.visibility === Window.FullScreen

                    // The viewer's keys, also while one of the strip's
                    // buttons has focus.
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

                    Components.ViewerStrip {
                        id: albumArtStrip
                        objectName: "albumArtFullscreenStrip"
                        anchors.fill: parent
                        uiPalette: root.uiPalette
                        uiBridge: root.uiBridge
                        positionText: root.positionText
                        closeButtonName: "albumArtFullscreenCloseButton"
                        zoomLevel: root.zoomLevel
                        resetView: root.resetView
                        resetToolTip: I18n.i18nc("@info:tooltip", "Fit the Image to the Screen")
                        errorController: root.errorController
                        closeViewer: root.closeViewer

                        InfoButton {
                            objectName: "albumArtFullscreenInfoButton"
                        }
                    }
                }
            }
        }
    }
}
