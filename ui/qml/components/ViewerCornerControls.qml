// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components

// The controls of the visualization viewer in the main window, which has
// no title strip: one plate in the top right corner, in the plot's colours.
// A zoom readout and Reset while the view is zoomed, the new issues while
// there are any, and Close, the same controls the strip of the viewer over
// the whole screen has: 24 px tool buttons, 2 px apart with 2 px around.
//
// The items are placed by bindings rather than the plate's Row: a
// positioner places an item that has just appeared only when the window
// next draws, so the readout and Reset would show where they were last,
// on a plate of the old width, until then.
Item {
    id: root

    required property var plotColors
    required property var uiPalette
    required property var closeViewer
    property bool controlsVisible: true
    property var pointerActivity: null
    property real zoomLevel: 1.0
    property var resetView: null
    property string resetToolTip: I18n.i18nc("@info:tooltip", "Reset Zoom")
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"

    readonly property bool zoomed: Math.abs(root.zoomLevel - 1.0) > 0.001
    readonly property int unseenIssueCount: root.errorController ? root.errorController.unseenCount : 0
    readonly property real spacing: 2
    readonly property bool readoutShown: root.zoomed
    readonly property bool resetShown: root.zoomed && root.resetView !== null
    readonly property bool issuesShown: root.unseenIssueCount > 0

    implicitWidth: closeButton.x + closeButton.width + root.spacing
    implicitHeight: 24 + 2 * root.spacing
    // As the title strip of the viewer over the whole screen: shown at once,
    // so the key that brings the controls back can move focus to them; it
    // fades out on the strip's curve, and hidden it takes no pointer.
    opacity: root.controlsVisible ? 1 : 0
    visible: root.controlsVisible || opacity > 0

    // Kirigami's durations are 0 with animations turned off.
    Behavior on opacity {
        enabled: Kirigami.Units.shortDuration > 0
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
        }
    }

    Accessible.role: Accessible.Grouping
    Accessible.name: I18n.i18nc("@label", "Viewer")

    HoverHandler {
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
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        takesPointer: true
        Accessible.ignored: true
    }

    Label {
        id: readout
        objectName: "viewerZoomReadout"
        x: root.spacing
        y: root.spacing
        visible: root.readoutShown
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
        id: resetButton
        objectName: "viewerResetButton"
        x: readout.x + (root.readoutShown ? readout.width + root.spacing : 0)
        y: root.spacing
        visible: root.resetShown
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: root.resetToolTip
        icon.name: "zoom-original"
        onClicked: root.resetView()
    }

    Components.PlotToolButton {
        id: issuesButton
        objectName: "viewerIssuesButton"
        x: resetButton.x + (root.resetShown ? resetButton.width + root.spacing : 0)
        y: root.spacing
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
        y: root.spacing
        plotColors: root.plotColors
        uiPalette: root.uiPalette
        text: I18n.i18nc("@action:button", "Close")
        icon.name: "window-close"
        onClicked: root.closeViewer()
    }
}
