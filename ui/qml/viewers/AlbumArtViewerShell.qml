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
    // A preview from Replace from iTunes, whose label and buttons are in
    // the image's details (AlbumArtSurface), which show throughout it: the
    // Image Details button hides meanwhile, and Escape goes back to the
    // results, as closeViewer does then.
    property bool previewActive: false
    // Whether the title strip shows over the whole screen when the pointer
    // moves: the Preferences setting.
    property bool stripEnabled: true

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
    // The title strip's height over the whole screen, which it takes of the
    // art's top edge while it shows; none while it is turned off.
    readonly property real stripHeight: root.fullscreenWindow && root.stripEnabled
        ? root.fullscreenWindow.strip.implicitHeight : 0
    property alias fullscreenControlsHideDelay: fullscreenControlsAutoHide.hideDelay
    readonly property bool fullscreenControlsVisible: fullscreenControlsAutoHide.controlsVisible

    // Over the whole screen, the title strip hides while the pointer rests,
    // as the visualization viewer's does, and stays while one of its buttons
    // has keyboard focus. In the main window, the controls stay.
    Components.FullscreenControlsAutoHide {
        id: fullscreenControlsAutoHide
        objectName: "albumArtFullscreenControlsAutoHide"
        active: root.wholeScreenOpen
        held: root.fullscreenWindow !== null && root.fullscreenWindow.strip.focusInside
    }

    // Made the first time the viewer opens over the whole screen and kept,
    // hidden while the viewer is closed, as the visualization viewer's is.
    onWholeScreenOpenChanged: {
        if (root.wholeScreenOpen) {
            albumArtFullscreenWindowLoader.active = true
        }
    }

    onViewerOpenChanged: Qt.callLater(root.syncPresentation)
    // A preview shows the viewer's controls and takes the keys from the
    // dialog it came from.
    onPreviewActiveChanged: {
        if (root.previewActive) {
            fullscreenControlsAutoHide.wake()
            Qt.callLater(root.focusView)
        }
    }
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
            root.focusView()
        }
    }

    function focusFullscreen() {
        if (root.wholeScreenVisible) {
            root.fullscreenWindow.focusSink.forceActiveFocus()
        }
    }

    // Gives the art the keys, away from the button that last had them, as
    // the viewer opens and as a preview begins: Space plays and pauses.
    function focusView() {
        if (root.wholeScreenVisible) {
            root.fullscreenWindow.viewFocus.forceActiveFocus()
        } else if (albumArtViewer.visible) {
            albumArtPopupViewFocus.forceActiveFocus()
        }
    }

    Component.onCompleted: {
        if (root.wholeScreenOpen) {
            albumArtFullscreenWindowLoader.active = true
        }
        Qt.callLater(root.syncPresentation)
    }

    // The info toggle: the tint while the details show, the ring only for
    // keyboard focus. Hidden during a preview, whose details always show,
    // it takes no room.
    component InfoButton: Components.ViewerToolButton {
        visible: !root.previewActive
        width: visible ? implicitWidth : 0
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
        // The style's own transitions.

        // Takes the keys of the viewer's items. During a preview, Escape goes
        // back to the results: closeViewer then leaves the viewer open if it
        // was before, so the popup does not close for it.
        contentItem: FocusScope {
            focus: true
            Keys.onEscapePressed: function(event) {
                event.accepted = root.previewActive
                if (root.previewActive) {
                    root.closeViewer()
                }
            }

            // Takes focus from the viewer's buttons when the art is pressed,
            // as the whole screen's does.
            Item {
                id: albumArtPopupViewFocus
                objectName: "albumArtPopupViewFocus"
                focus: true
            }
        }

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

        // The popup is kept: it opens with the art's focus, not that of the
        // button that had it as it last closed.
        onAboutToShow: albumArtPopupViewFocus.focus = true

        onClosed: {
            if (root.viewerOpen && !root.useWholeScreenViewerMode) {
                root.closeViewer()
                // Back from a preview to a viewer that stays open.
                if (root.viewerOpen) {
                    Qt.callLater(root.syncPresentation)
                }
            }
        }

        // The art on the view colour.
        background: Rectangle {
            color: root.uiPalette.view
        }

        // The art, under the controls' plate.
        Item {
            anchors.fill: parent

            Item {
                id: albumArtPopupHost
                anchors.fill: parent
            }

            // A press anywhere on the art takes the focus from the viewer's
            // buttons, so Space plays and pauses again, then goes on to the
            // art.
            MouseArea {
                anchors.fill: parent
                z: 1
                acceptedButtons: Qt.AllButtons
                onPressed: function(mouse) {
                    albumArtPopupViewFocus.forceActiveFocus(Qt.MouseFocusReason)
                    mouse.accepted = false
                }
            }
        }

        // The window's title names the viewer, so it has no title strip:
        // its controls are on a plate in the top right corner, on the art.
        // The plate is sized from the controls, which place themselves at
        // once, rather than by the plate's Row, which would wait for the
        // next frame.
        Item {
            objectName: "albumArtPopupControls"
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 4
            implicitWidth: popupControls.implicitWidth + 4
            implicitHeight: popupControls.implicitHeight + 4

            Components.PlotPlate {
                anchors.fill: parent
                plotColors: root.uiPalette.waveformPlot
                uiPalette: root.uiPalette
                takesPointer: true
                Accessible.ignored: true
            }

            Components.ViewerControls {
                id: popupControls
                x: 2
                y: 2
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
                readonly property Item viewFocus: albumArtFullscreenViewFocus
                readonly property Item strip: albumArtStrip
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
                        root.focusView()
                    }
                }

                onClosing: function(close) {
                    if (root.viewerOpen && root.useWholeScreenViewerMode) {
                        root.closeViewer()
                        // Back from a preview to a viewer that stays open.
                        close.accepted = !root.viewerOpen
                    }
                }

                Shortcut {
                    sequence: "I"
                    context: Qt.WindowShortcut
                    enabled: albumArtFullscreenWindow.visibility === Window.FullScreen
                    onActivated: root.toggleInfoVisible()
                }

                // The whole viewer: its keys reach every control in it, in
                // the strip and in the details beside the art alike.
                FocusScope {
                    id: albumArtFullscreenFocusSink
                    objectName: "albumArtFullscreenFocusScope"
                    anchors.fill: parent
                    focus: albumArtFullscreenWindow.visibility === Window.FullScreen

                    // Takes focus from the strip when the art is pressed.
                    Item {
                        id: albumArtFullscreenViewFocus
                        objectName: "albumArtFullscreenViewFocus"
                        focus: true
                    }

                    // The pointer anywhere in the viewer brings the strip
                    // back; the art keeps its own hover.
                    HoverHandler {
                        onPointChanged: fullscreenControlsAutoHide.pointerMoved(
                            point.scenePosition.x, point.scenePosition.y)
                    }

                    // Any key brings the strip back, so Tab reaches it.
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Escape) {
                            event.accepted = true
                            root.closeViewer()
                            return
                        }
                        fullscreenControlsAutoHide.wake()
                        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                            event.accepted = true
                            root.switchComparisonImage()
                        } else if (event.key === Qt.Key_Menu
                                || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                            event.accepted = true
                            root.openContextMenu()
                        }
                    }

                    // The art, over the whole window: the title strip is
                    // drawn over its top edge.
                    Item {
                        anchors.fill: parent

                        Item {
                            id: albumArtWindowHost
                            anchors.fill: parent
                        }

                        // A press anywhere on the art takes the focus from
                        // the strip, so it can hide again, then goes on to
                        // the art.
                        MouseArea {
                            anchors.fill: parent
                            z: 1
                            acceptedButtons: Qt.AllButtons
                            onPressed: function(mouse) {
                                albumArtFullscreenViewFocus.forceActiveFocus(Qt.MouseFocusReason)
                                mouse.accepted = false
                            }
                        }
                    }

                    Components.ViewerStrip {
                        id: albumArtStrip
                        objectName: "albumArtFullscreenStrip"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        z: 1
                        uiPalette: root.uiPalette
                        // The art is on the view colour, as the waveform is.
                        plotColors: root.uiPalette.waveformPlot
                        uiBridge: root.uiBridge
                        positionText: root.positionText
                        closeButtonName: "albumArtFullscreenCloseButton"
                        controlsVisible: root.fullscreenControlsVisible
                        available: root.stripEnabled
                        pointerActivity: fullscreenControlsAutoHide.pointerMoved
                        zoomLevel: root.zoomLevel
                        resetView: root.resetView
                        resetToolTip: I18n.i18nc("@info:tooltip", "Fit the Image to the Screen")
                        errorController: root.errorController
                        closeViewer: root.closeViewer

                        InfoButton {
                            objectName: "albumArtFullscreenInfoButton"
                            plotColors: albumArtStrip.plotColors
                        }
                    }
                }
            }
        }
    }
}
