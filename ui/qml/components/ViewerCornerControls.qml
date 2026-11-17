// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components

// The controls of the visualization viewer in the main window, which has
// no title strip: in the top right corner, in the plot's colours, right of
// the plot's mode switch, on the one plate the plot layer draws under both
// with `bare` (#373 H37), or on a plate of their own. The new issues while there are any, and
// Close: 24 px tool buttons, 4 px apart with 3 px around, as the mode
// switch's beside it, so a focus ring stays inside the plate and clear of
// the other button (#373 H14 G4). The zoom is on the
// plot, left of the mode switch, as in the main pane and over the whole
// screen: [Zoom ↺] [mode] [✕] everywhere.
//
// The items are placed by bindings rather than the plate's Row: a
// positioner places an item that has just appeared only when the window
// next draws, so Close would show where it was last, on a plate of the old
// width, until then.
Item {
    id: root

    required property var plotColors
    required property var uiPalette
    required property var closeViewer
    property bool controlsVisible: true
    // The controls as they fade (FullscreenControlsAutoHide's level); at
    // once without one.
    property real controlsLevel: root.controlsVisible ? 1 : 0
    property var pointerActivity: null
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"
    // Without a plate of its own, on one the plot layer draws under it and
    // the mode switch (#373 H37).
    property bool bare: false

    readonly property int unseenIssueCount: root.errorController ? root.errorController.unseenCount : 0
    // As the mode switch's: a focus ring stays 1 px inside the plate.
    readonly property real padding: 4
    readonly property real spacing: 4
    readonly property bool issuesShown: root.unseenIssueCount > 0

    implicitWidth: closeButton.x + closeButton.width + root.padding
    implicitHeight: root.uiPalette.toolButtonSize + 2 * root.padding
    // As the title strip of the viewer over the whole screen: shown at once,
    // so the key that brings the controls back can move focus to them; it
    // fades out on the strip's curve, and hidden it takes no pointer.
    opacity: root.controlsLevel
    visible: root.controlsVisible || root.controlsLevel > 0

    Accessible.role: Accessible.Grouping
    Accessible.name: I18n.i18nc("@label", "Viewer")

    // Whether the pointer is on the plate, which counts as on the view.
    readonly property bool hovered: cornerHover.hovered

    HoverHandler {
        id: cornerHover
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    // The plate, under the controls; presses between them stay on it.
    Components.PlotPlate {
        objectName: "viewerCornerPlate"
        anchors.fill: parent
        bare: root.bare
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        takesPointer: true
        Accessible.ignored: true
    }

    Components.PlotToolButton {
        id: issuesButton
        objectName: "viewerIssuesButton"
        x: root.padding
        y: root.padding
        visible: root.issuesShown
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: I18n.i18nc("@info:tooltip %1 is the issue count", "%1: Show Recent Issues",
            I18n.i18ncp("@action:button", "%1 new issue", "%1 new issues", root.unseenIssueCount))
        icon.name: Qt.resolvedUrl("../assets/status-warning.svg")
        onClicked: root.errorController.historyRequested()
    }

    Components.PlotToolButton {
        id: closeButton
        objectName: root.closeButtonName
        x: issuesButton.x + (root.issuesShown ? issuesButton.width + root.spacing : 0)
        y: root.padding
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: I18n.i18nc("@action:button", "Close")
        icon.name: "window-close"
        onClicked: root.closeViewer()
    }
}
