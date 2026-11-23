// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/PlotLanes.js" as PlotLanes
import "../logic/VisualizationModes.js" as VisualizationModes

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
    // What the level meter's column takes of the view's right edge, which
    // the corner's plate keeps clear of.
    property real meterWidth: 0
    // The corner plate's inset from the plot's corner, as the lane heads'.
    readonly property int cornerInset: 5
    // Its inset from the plot's right edge: the corner's, or beside the
    // level meter's column, 8 px from it (at 5 px, its edge all but met the
    // meter's seam).
    readonly property int cornerRightInset: root.meterWidth > 0 ? 8 : root.cornerInset
    // Whether the title strip shows over the whole screen when the pointer
    // moves: the Preferences setting.
    property bool stripEnabled: true
    // The plot's zoom, 1 at the default, and what resets it, for the strip.
    property real zoomLevel: 1.0
    property var resetZoom: null
    // Whether the view is moved away from the playhead, which the controls
    // say, hiding as they do (#467), and Follow Playhead's shortcut.
    property bool viewMoved: false
    property string followShortcut: ""

    property alias popupHost: spectrogramPopupHost
    // The visualization, whose last frame the viewer in the main window
    // keeps as it fades out after it closes (the visualization went
    // back to the pane the moment the viewer closed, and the viewer's black
    // floor faded out alone over the pane). The pane shows the visualization
    // live under it meanwhile, as the pane's own last frame shows under the
    // viewer as it opens. The frame is grabbed once, while the
    // visualization is still in the viewer: until then `keepsVisualization`
    // tells Main.qml to leave it there. It is a ShaderEffectSource of the
    // visualization, made only while the viewer fades out, in the main
    // window, and given up before the visualization may go into the
    // whole-screen window: `holdsVisualization` tells Main.qml not to take
    // it there until then (see SpectrogramPane).
    property Item visualization: null
    // The plot's pointer, which the areas over it show: hidden with the
    // controls, or a pan's closed hand.
    readonly property int plotCursorShape: root.visualization?.pointerCursorShape ?? Qt.ArrowCursor
    // Whether the viewer in the main window shows: while it is open, and
    // while it fades out after it closes.
    readonly property bool popupShown: spectrogramViewer.visible
    readonly property bool keepsVisualization: spectrogramViewer.visible
        && !(closingFrameLoader.item !== null && closingFrameLoader.item.grabbed)
    readonly property bool holdsVisualization: closingFrameLoader.item !== null
    // Whether it keeps a frame: closed, while it still shows in a window
    // that draws.
    readonly property bool keepsClosingFrame: !root.viewerOpen && spectrogramViewer.visible
        && root.windowRoot !== null
        && root.windowRoot.visibility !== Window.Hidden && root.windowRoot.visibility !== Window.Minimized
    onKeepsClosingFrameChanged: root.updateClosingFrame()

    Connections {
        target: root.visualization
        function onParentChanged() { root.updateClosingFrame() }
    }

    // Makes the frame once the viewer is to keep one and the visualization
    // is in it to grab; lets the visualization go and destroys the frame as
    // soon as it is not to.
    function updateClosingFrame() {
        if (!root.keepsClosingFrame) {
            if (closingFrameLoader.item !== null) {
                closingFrameLoader.item.sourceItem = null
            }
            closingFrameLoader.active = false
            return
        }
        if (closingFrameLoader.active || root.visualization === null
                || root.visualization.parent !== spectrogramPopupHost) {
            return
        }
        closingFrameLoader.active = true
        const frame = closingFrameLoader.item
        frame.keptSize = Qt.size(spectrogramPopupHost.width, spectrogramPopupHost.height)
        frame.sourceItem = root.visualization
        frame.scheduleUpdate()
    }
    // The window of the viewer over the whole screen, and the item in it
    // that holds the visualization; null until the viewer first opens there.
    readonly property Window fullscreenWindow: spectrogramFullscreenWindowLoader.item
    readonly property Item windowHost: root.fullscreenWindow ? root.fullscreenWindow.host : null
    property alias fullscreenControlsHideDelay: fullscreenControlsAutoHide.hideDelay
    // The corner plate that holds the viewer's controls: in the window, and
    // over the whole screen with the title strip turned off; none with the
    // strip on, which holds them (the plot's plate under the strip's Close
    // read as a second cluster).
    readonly property Item cornerControls: !root.useWholeScreenViewerMode
        ? popupControls
        : (root.fullscreenWindow && !root.stripEnabled ? root.fullscreenWindow.corner : null)
    // What the visualization keeps clear of in the plot's top right corner,
    // meter's column apart: that plate, with the 1 px edge outside it. It
    // does not change as it auto-hides.
    readonly property size reservedCorner: root.cornerControls
        ? Qt.size(root.cornerRightInset + root.cornerControls.width + 1,
                  root.cornerInset + root.cornerControls.height + 1)
        : Qt.size(0, 0)
    // How far the visualization's controls, such as the first lane's Mute
    // and Solo, keep below its top edge: over the whole
    // screen, the title strip's height while it can show. They show only
    // with the viewer's controls, so with the strip over the top edge; they
    // keep there while it is hidden, rather than move each time it shows.
    // What only reads, such as the time scale, stays on the edge and shows
    // while it is hidden. With the strip turned off, they are on the edge.
    readonly property real controlsTopInset: root.wholeScreenOpen && root.fullscreenWindow
            && root.stripEnabled
        ? root.fullscreenWindow.strip.implicitHeight : 0
    readonly property bool fullscreenControlsVisible: fullscreenControlsAutoHide.controlsVisible
    // Whether the pointer is on the corner plate, where the plot's crosshair
    // gives way as it does on the plot's own plates.
    readonly property bool cornerHovered: root.viewerOpen && root.cornerControls !== null
        && root.cornerControls.hovered
    // The controls as they fade: 1 shown, 0 hidden. Everything that hides
    // with them takes it, the strip, the corner controls, the mode switch,
    // the lanes' buttons and what the plot draws for the pointer.
    readonly property real fullscreenControlsLevel: fullscreenControlsAutoHide.level
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
        // The controls stay while one of them has focus from the keyboard
        // or assistive technology; the focus a click gives a button lets
        // them hide after the pointer rests, as before the click.
        held: root.focusedItem !== root.viewFocus
            && root.isInside(root.focusedItem, root.viewerScope)
            && root.focusedItem.focusReason !== Qt.MouseFocusReason
    }

    // Sees a press on the view and where it moves, whatever takes it, for a
    // drag to count as pointer activity. It is on the area over the view
    // that has every press first: the view's items, which take it, keep it
    // from handlers on what is under them.
    component DragWatch: PointHandler {
        acceptedButtons: Qt.AllButtons
        onActiveChanged: fullscreenControlsAutoHide.pressTracked(
            active, point.scenePosition, point.scenePressPosition)
        onPointChanged: fullscreenControlsAutoHide.pressTracked(
            active, point.scenePosition, point.scenePressPosition)
    }

    // The viewer's mode switch, in the strip and on the corner plate: the
    // current mode checked, in the look of a choice that is on, as the
    // album art viewer's info toggle. The two are radio buttons to
    // assistive technology, as on the main pane's plot.
    component ModeSwitch: Row {
        id: modeSwitch

        required property var plotColors
        required property string buttonName

        spacing: 4
        Accessible.role: Accessible.Grouping
        Accessible.name: I18n.i18nc("@label", "Visualization")

        Repeater {
            model: VisualizationModes.modes(String(Qt.resolvedUrl("../assets/")))

            delegate: Components.ViewerToolButton {
                required property int index
                required property var modelData
                objectName: modeSwitch.buttonName + index
                uiPalette: root.uiPalette
                plotColors: modeSwitch.plotColors
                text: modelData.name
                icon.name: modelData.iconName
                icon.source: modelData.iconSource
                // Its owner binds `checked` to the mode and chooses on a
                // click; choosing the chosen one again leaves it chosen.
                checkable: true
                autoExclusive: true
                checked: root.visualizationMode === index
                Accessible.role: Accessible.RadioButton
                Accessible.checkable: true
                Accessible.checked: checked
                Accessible.onPressAction: root.setVisualizationMode(index)
                onClicked: root.setVisualizationMode(index)
            }
        }
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
        // The style's own transitions, of one picture: the content below.
        onClosed: {
            if (root.viewerOpen && !root.useWholeScreenViewerMode) {
                root.closeViewer()
            }
        }
        // Nothing of its own, as the album art viewer's: the view's floor is
        // the one floor, in the content's layer (a second one here, fading
        // apart from it, took the window down to 79 % of the darker of the
        // two pictures the fade is between).
        background: Item {
            objectName: "spectrogramPopupBackground"
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
            // The floor, the plot and the plates fade as one picture on the
            // window under them, as the plots do in the mode switch: without
            // the layer each is at the popup's opacity alone, and the floor
            // shows through all that is on it. Only while the popup fades.
            layer.enabled: spectrogramViewer.opacity > 0 && spectrogramViewer.opacity < 1

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

            // Above the view, and before it in the Tab order: the modes
            // come before the lanes' Mute and Solo, as in the main pane.
            Components.ViewerCornerControls {
                id: popupControls
                objectName: "spectrogramPopupControls"
                z: 20
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: root.cornerInset
                anchors.rightMargin: root.meterWidth + root.cornerRightInset
                plotColors: root.uiPalette.plotColors(root.visualizationMode)
                uiPalette: root.uiPalette
                closeButtonName: "spectrogramPopupCloseButton"
                controlsVisible: root.fullscreenControlsVisible
                controlsLevel: root.fullscreenControlsLevel
                pointerActivity: root.noteFullscreenPointerActivity
                errorController: root.errorController
                closeViewer: root.closeViewer
                zoomLevel: root.zoomLevel
                resetView: root.resetZoom
                noticeShown: root.viewMoved
                followShortcut: root.followShortcut

                ModeSwitch {
                    objectName: "spectrogramPopupModeSwitch"
                    buttonName: "spectrogramPopupModeButton"
                    plotColors: popupControls.plotColors
                }
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

                // The visualization's last frame as the viewer fades out,
                // under the host, where the visualization covers it until it
                // goes back to the pane.
                Loader {
                    id: closingFrameLoader
                    active: false
                    sourceComponent: ShaderEffectSource {
                        id: closingFrame
                        objectName: "spectrogramViewerClosingFrame"
                        property bool grabbed: false
                        property size keptSize: Qt.size(0, 0)
                        width: closingFrame.keptSize.width
                        height: closingFrame.keptSize.height
                        live: false
                        hideSource: false
                        sourceRect: Qt.rect(0, 0, closingFrame.keptSize.width, closingFrame.keptSize.height)
                        onScheduledUpdateCompleted: closingFrame.grabbed = closingFrame.sourceItem !== null
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
                    cursorShape: root.plotCursorShape
                    acceptedButtons: Qt.AllButtons
                    onPressed: function(mouse) {
                        spectrogramPopupViewFocus.forceActiveFocus(Qt.MouseFocusReason)
                        mouse.accepted = false
                    }

                    DragWatch {}
                }
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
                readonly property Item corner: spectrogramFullscreenCorner
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

                    // With the strip turned off, the modes and the zoom are
                    // on a plate in the corner, the strip's own buttons; its
                    // Close is the keys' and the double-click's then.
                    Components.ViewerCornerControls {
                        id: spectrogramFullscreenCorner
                        objectName: "spectrogramFullscreenCorner"
                        z: 1
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: root.cornerInset
                        anchors.rightMargin: root.meterWidth + root.cornerRightInset
                        available: !root.stripEnabled
                        closeShown: false
                        closeButtonName: "spectrogramFullscreenCornerCloseButton"
                        plotColors: root.uiPalette.plotColors(root.visualizationMode)
                        uiPalette: root.uiPalette
                        controlsVisible: root.fullscreenControlsVisible
                        controlsLevel: root.fullscreenControlsLevel
                        pointerActivity: root.noteFullscreenPointerActivity
                        closeViewer: root.closeViewer
                        zoomLevel: root.zoomLevel
                        resetView: root.resetZoom
                        noticeShown: root.viewMoved
                        followShortcut: root.followShortcut

                        ModeSwitch {
                            objectName: "spectrogramFullscreenCornerModeSwitch"
                            buttonName: "spectrogramFullscreenCornerModeButton"
                            plotColors: spectrogramFullscreenCorner.plotColors
                        }
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
                            cursorShape: root.plotCursorShape
                            acceptedButtons: Qt.AllButtons
                            onPressed: function(mouse) {
                                spectrogramFullscreenViewFocus.forceActiveFocus(Qt.MouseFocusReason)
                                mouse.accepted = false
                            }

                            DragWatch {}
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
                        controlsLevel: root.fullscreenControlsLevel
                        trackLevel: PlotLanes.stripTextLevel(root.fullscreenControlsLevel)
                        available: root.stripEnabled
                        pointerActivity: root.noteFullscreenPointerActivity
                        errorController: root.errorController
                        closeViewer: root.closeViewer
                        // The plot's zoom, the mode switch and Close, each
                        // set off by a 1 px divider, as on the corner plate.
                        zoomLevel: root.zoomLevel
                        resetView: root.resetZoom
                        noticeShown: root.viewMoved
                        followShortcut: root.followShortcut
                        dividers: true
                        closeDivided: true

                        ModeSwitch {
                            objectName: "spectrogramStripModeSwitch"
                            buttonName: "spectrogramStripModeButton"
                            plotColors: spectrogramStrip.plotColors
                        }
                    }
                }
            }
        }
    }
}
