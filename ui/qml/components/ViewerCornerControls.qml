// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import FerrousUi 1.0
import "." as Components

// The controls of a viewer in the main window, which has no title strip:
// one plate in the top right corner, in the plot's colours. A zoom readout
// and Reset while the view is zoomed, the new issues while there are any,
// the viewer's own buttons (declared inside it) and Close, the same
// controls the strip of the viewer over the whole screen has.
Components.PlotPlate {
    id: root

    required property var closeViewer
    property bool controlsVisible: true
    property var pointerActivity: null
    property real zoomLevel: 1.0
    property var resetView: null
    property string resetToolTip: I18n.i18nc("@info:tooltip", "Reset Visualization View")
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"
    default property alias viewerControls: viewerControlsRow.data

    readonly property bool zoomed: Math.abs(root.zoomLevel - 1.0) > 0.001
    readonly property int unseenIssueCount: root.errorController ? root.errorController.unseenCount : 0

    visible: root.controlsVisible
    takesPointer: true
    Accessible.name: I18n.i18nc("@label", "Viewer")

    HoverHandler {
        onPointChanged: {
            if (root.pointerActivity) {
                root.pointerActivity(point.scenePosition.x, point.scenePosition.y)
            }
        }
    }

    Label {
        objectName: "viewerZoomReadout"
        visible: root.zoomed
        height: 24
        leftPadding: 4
        rightPadding: 2
        verticalAlignment: Text.AlignVCenter
        text: I18n.i18nc("@info:status zoom of the view in percent", "Zoom %1%",
            Math.round(root.zoomLevel * 100))
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
        color: root.plotColors.plotTextSecondary
    }

    Components.PlotToolButton {
        objectName: "viewerResetButton"
        visible: root.zoomed && root.resetView !== null
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: root.resetToolTip
        icon.name: "zoom-original"
        onClicked: root.resetView()
    }

    Components.PlotToolButton {
        objectName: "viewerIssuesButton"
        visible: root.unseenIssueCount > 0
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: I18n.i18nc("@info:tooltip %1 is the issue count", "%1: Show Recent Issues",
            I18n.i18ncp("@action:button", "%1 new issue", "%1 new issues", root.unseenIssueCount))
        icon.name: Qt.resolvedUrl("../assets/status-warning.svg")
        onClicked: root.errorController.historyRequested()
    }

    Row {
        id: viewerControlsRow
        spacing: 2
    }

    Components.PlotToolButton {
        objectName: root.closeButtonName
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: I18n.i18nc("@action:button", "Close")
        icon.name: "window-close"
        onClicked: root.closeViewer()
    }
}
