// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components

// The controls of a viewer, as flat 24 px tool buttons 4 px apart on the
// window's colours: a quiet zoom readout and Reset while the view is zoomed,
// a button to Recent Issues while there are new ones (the viewer covers
// the status bar), the viewer's own buttons (declared inside) and Close.
// They fade out as the viewer's controls auto-hide.
Row {
    id: root

    required property var uiPalette
    required property var closeViewer
    property bool controlsVisible: true
    // The view's zoom, 1 at the default; Reset calls resetView.
    property real zoomLevel: 1.0
    property var resetView: null
    property string resetToolTip: I18n.i18nc("@info:tooltip", "Reset Visualization View")
    property var errorController: null
    property string closeButtonName: "viewerCloseButton"
    default property alias viewerControls: viewerControlsRow.data

    readonly property bool zoomed: Math.abs(root.zoomLevel - 1.0) > 0.001
    readonly property int unseenIssueCount: root.errorController ? root.errorController.unseenCount : 0

    spacing: 4
    opacity: root.controlsVisible ? 1 : 0
    // Shown at once, so the key that brings the controls back can move
    // focus to them; they fade out.
    visible: root.controlsVisible || opacity > 0

    Behavior on opacity {
        enabled: Kirigami.Units.shortDuration > 0
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
        }
    }

    // Quiet: only while the view is zoomed.
    Label {
        objectName: "viewerZoomReadout"
        visible: root.zoomed
        height: 24
        rightPadding: 4
        verticalAlignment: Text.AlignVCenter
        text: I18n.i18nc("@info:status zoom of the view in percent", "Zoom %1%",
            Math.round(root.zoomLevel * 100))
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
        color: root.uiPalette.textSecondary
    }

    Components.ViewerToolButton {
        objectName: "viewerResetButton"
        visible: root.zoomed && root.resetView !== null
        uiPalette: root.uiPalette
        display: AbstractButton.TextOnly
        text: I18n.i18nc("@action:button reset the view's zoom", "Reset")
        toolTipText: root.resetToolTip
        onClicked: root.resetView()
    }

    Components.ViewerToolButton {
        objectName: "viewerIssuesButton"
        visible: root.unseenIssueCount > 0
        uiPalette: root.uiPalette
        text: I18n.i18ncp("@action:button", "%1 new issue", "%1 new issues",
            root.unseenIssueCount)
        toolTipText: I18n.i18nc("@info:tooltip %1 is the issue count", "%1: Show Recent Issues",
            text)
        icon.name: Qt.resolvedUrl("../assets/status-warning.svg")
        glyphColor: root.uiPalette.negative
        onClicked: root.errorController.historyRequested()
    }

    Row {
        id: viewerControlsRow
        spacing: 4
    }

    Components.ViewerToolButton {
        objectName: root.closeButtonName
        uiPalette: root.uiPalette
        text: I18n.i18nc("@action:button", "Close")
        icon.name: "window-close"
        onClicked: root.closeViewer()
    }
}
