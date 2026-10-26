// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Window 2.15

// The main window's layout: its geometry, whether it is maximized, the
// shares of the library pane and the queue, the visualization mode, and
// whether the queue follows the current track. The backend saves it with
// the settings, and the window opens with what was saved.
QtObject {
    id: root

    required property var uiBridge
    required property var window
    required property var viewerController
    required property var queueController

    // What was saved, read once as the window is created, before it is
    // shown; see BridgeClient::savedWindowLayout.
    readonly property var saved: root.uiBridge.savedWindowLayout()
    readonly property bool hasSavedGeometry: root.saved.width !== undefined
    readonly property int initialWidth: root.hasSavedGeometry ? root.saved.width : 1600
    readonly property int initialHeight: root.hasSavedGeometry ? root.saved.height : 980
    readonly property bool initiallyMaximized: root.saved.maximized === true
    readonly property int initialVisualizationMode: root.saved.visualizationMode === 1 ? 1 : 0
    readonly property bool initialFollowCurrentTrack: root.saved.followCurrentTrack !== false

    // The share of the split's width the library pane takes, and of the
    // height the queue shares with the visualization, as the user last
    // dragged them; 0 until then, for the default sizes. A share rather
    // than a size, so the panes keep their proportions as the window is
    // resized.
    property real sidebarWidthRatio: root.saved.sidebarWidthRatio !== undefined
        ? root.saved.sidebarWidthRatio
        : 0
    property real queueHeightRatio: root.saved.queueHeightRatio !== undefined
        ? root.saved.queueHeightRatio
        : 0

    // The window's geometry when it was last neither maximized, minimized,
    // nor fullscreen: what it returns to, and what is saved.
    property rect normalGeometry: root.hasSavedGeometry
        ? Qt.rect(root.saved.x, root.saved.y, root.saved.width, root.saved.height)
        : Qt.rect(0, 0, 0, 0)

    function clamp(value, minimum, maximum) {
        return Math.max(minimum, Math.min(maximum, value))
    }

    // The library pane's width in a split `splitWidth` wide, leaving the
    // pane after it `otherMinimum`.
    function sidebarWidth(splitWidth, minimum, otherMinimum, defaultWidth) {
        const preferred = root.sidebarWidthRatio > 0
            ? splitWidth * root.sidebarWidthRatio
            : defaultWidth
        return Math.round(root.clamp(preferred, minimum, Math.max(minimum, splitWidth - otherMinimum)))
    }

    // The queue's height in a split `splitHeight` high, leaving the
    // visualization `otherMinimum`.
    function queueHeight(splitHeight, minimum, otherMinimum, defaultHeight) {
        const preferred = root.queueHeightRatio > 0
            ? splitHeight * root.queueHeightRatio
            : defaultHeight
        return Math.round(root.clamp(preferred, minimum, Math.max(minimum, splitHeight - otherMinimum)))
    }

    // A handle was dragged to give the pane `size` of `splitSize`.
    function noteSidebarWidth(size, splitSize) {
        if (splitSize > 0 && size > 0) {
            root.sidebarWidthRatio = size / splitSize
            root.persist()
        }
    }

    function noteQueueHeight(size, splitSize) {
        if (splitSize > 0 && size > 0) {
            root.queueHeightRatio = size / splitSize
            root.persist()
        }
    }

    function noteWindowGeometry() {
        if (root.window.visibility === Window.Windowed) {
            root.normalGeometry = Qt.rect(
                root.window.x, root.window.y, root.window.width, root.window.height)
        }
    }

    function layout() {
        const geometry = root.normalGeometry
        const result = {
            "maximized": root.window.visibility === Window.Maximized,
            "visualizationMode": root.viewerController.visualizationMode,
            "followCurrentTrack": root.queueController.autoCenterSelection
        }
        if (geometry.width > 0 && geometry.height > 0) {
            result.x = geometry.x
            result.y = geometry.y
            result.width = geometry.width
            result.height = geometry.height
        }
        if (root.sidebarWidthRatio > 0) {
            result.sidebarWidthRatio = root.sidebarWidthRatio
        }
        if (root.queueHeightRatio > 0) {
            result.queueHeightRatio = root.queueHeightRatio
        }
        return result
    }

    // Hand the layout to the backend, which saves it with the settings.
    function persist() {
        root.noteWindowGeometry()
        root.uiBridge.setWindowLayout(root.layout())
    }

    property Connections _windowConnections: Connections {
        target: root.window
        function onXChanged() { root.noteWindowGeometry() }
        function onYChanged() { root.noteWindowGeometry() }
        function onWidthChanged() { root.noteWindowGeometry() }
        function onHeightChanged() { root.noteWindowGeometry() }
        function onVisibilityChanged() {
            if (root.window.visibility === Window.Windowed
                    || root.window.visibility === Window.Maximized) {
                root.persist()
            }
        }
    }

    property Connections _viewerConnections: Connections {
        target: root.viewerController
        function onVisualizationModeChanged() { root.persist() }
    }

    property Connections _queueConnections: Connections {
        target: root.queueController
        function onAutoCenterSelectionChanged() { root.persist() }
    }
}
