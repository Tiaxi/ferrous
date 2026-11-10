// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import FerrousUi 1.0
import "." as Components
import "../logic/ColorUtils.js" as ColorUtils

// The controls of a viewer, as flat 24 px tool buttons 4 px apart on the
// window's colours, or with `plotColors` on the plot's: a quiet zoom readout
// and Reset while the view is zoomed, a button to Recent Issues while there
// are new ones (the viewer covers the status bar), the viewer's own buttons
// (declared inside, which take `plotColors` themselves) and Close.
//
// The items are placed by bindings rather than a Row: a positioner places
// an item that has just appeared only when the window next draws, so the
// readout and Reset would show where they were last until then.
Item {
    id: root

    required property var uiPalette
    required property var closeViewer
    // The plot's colours (UiPalette's PlotColors), or null for the window's.
    property var plotColors: null
    // The view's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    property string resetToolTip: I18n.i18nc("@info:tooltip", "Reset Zoom")
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"
    // The viewer's own button before Close, if it has one.
    default property alias viewerControls: viewerControlsRow.data

    readonly property bool zoomed: Math.abs(root.zoomLevel - 1.0) > 0.001
    readonly property int unseenIssueCount: root.errorController ? root.errorController.unseenCount : 0
    readonly property real spacing: 4
    // Which items take room, whatever the auto-hide does.
    readonly property bool readoutShown: root.zoomed
    readonly property bool resetShown: root.zoomed && root.resetView !== null
    readonly property bool issuesShown: root.unseenIssueCount > 0
    readonly property bool viewerControlsShown: viewerControlsRow.implicitWidth > 0

    implicitWidth: closeButton.x + closeButton.width
    implicitHeight: root.uiPalette.toolButtonSize

    // Quiet: only while the view is zoomed.
    Label {
        id: readout
        objectName: "viewerZoomReadout"
        x: 0
        visible: root.readoutShown
        height: root.uiPalette.toolButtonSize
        rightPadding: 4
        verticalAlignment: Text.AlignVCenter
        text: I18n.i18nc("@info:status zoom of the view in percent", "Zoom %1%",
            Math.round(root.zoomLevel * 100))
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
        color: root.plotColors ? root.plotColors.plotTextSecondary : root.uiPalette.textSecondary
    }

    Components.ViewerToolButton {
        id: resetButton
        objectName: "viewerResetButton"
        x: readout.x + (root.readoutShown ? readout.width + root.spacing : 0)
        visible: root.resetShown
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        display: AbstractButton.TextOnly
        text: I18n.i18nc("@action:button reset the view's zoom", "Reset")
        toolTipText: root.resetToolTip
        onClicked: root.resetView()
    }

    Components.ViewerToolButton {
        id: issuesButton
        objectName: "viewerIssuesButton"
        x: resetButton.x + (root.resetShown ? resetButton.width + root.spacing : 0)
        visible: root.issuesShown
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        text: I18n.i18ncp("@action:button", "%1 new issue", "%1 new issues",
            root.unseenIssueCount)
        toolTipText: I18n.i18nc("@info:tooltip %1 is the issue count", "%1: Show Recent Issues",
            text)
        icon.name: Qt.resolvedUrl("../assets/status-warning.svg")
        // The window's negative is lightened on dark and darkened on light:
        // on a plot of the other brightness, such as the spectrogram's black
        // in a light theme, the glyph is the plot's text instead.
        glyphColor: !root.plotColors
                || (ColorUtils.colorLuma(root.plotColors.plotBg) < 0.45) === root.uiPalette.themeIsDark
            ? root.uiPalette.negative
            : root.plotColors.plotText
        onClicked: root.errorController.historyRequested()
    }

    // The viewer's own button, such as the album art viewer's info toggle.
    Item {
        id: viewerControlsRow
        x: issuesButton.x + (root.issuesShown ? issuesButton.width + root.spacing : 0)
        implicitWidth: childrenRect.width
        implicitHeight: root.uiPalette.toolButtonSize
        width: implicitWidth
        height: implicitHeight
    }

    Components.ViewerToolButton {
        id: closeButton
        objectName: root.closeButtonName
        x: viewerControlsRow.x + (root.viewerControlsShown ? viewerControlsRow.implicitWidth + root.spacing : 0)
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        text: I18n.i18nc("@action:button", "Close")
        icon.name: "window-close"
        onClicked: root.closeViewer()
    }
}
