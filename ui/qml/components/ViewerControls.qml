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
//
// The items are placed by bindings rather than a Row: a positioner places
// an item that has just appeared only when the window next draws, so the
// readout and Reset would show where they were last until then. Hiding with
// the auto-hide leaves the width as it is, so nothing beside them moves.
Item {
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
    implicitHeight: 24
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
        id: readout
        objectName: "viewerZoomReadout"
        x: 0
        visible: root.readoutShown
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
        id: resetButton
        objectName: "viewerResetButton"
        x: readout.x + (root.readoutShown ? readout.width + root.spacing : 0)
        visible: root.resetShown
        uiPalette: root.uiPalette
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
        text: I18n.i18ncp("@action:button", "%1 new issue", "%1 new issues",
            root.unseenIssueCount)
        toolTipText: I18n.i18nc("@info:tooltip %1 is the issue count", "%1: Show Recent Issues",
            text)
        icon.name: Qt.resolvedUrl("../assets/status-warning.svg")
        glyphColor: root.uiPalette.negative
        onClicked: root.errorController.historyRequested()
    }

    // The viewer's own button, such as the album art viewer's info toggle.
    Item {
        id: viewerControlsRow
        x: issuesButton.x + (root.issuesShown ? issuesButton.width + root.spacing : 0)
        implicitWidth: childrenRect.width
        implicitHeight: 24
        width: implicitWidth
        height: implicitHeight
    }

    Components.ViewerToolButton {
        id: closeButton
        objectName: root.closeButtonName
        x: viewerControlsRow.x + (root.viewerControlsShown ? viewerControlsRow.implicitWidth + root.spacing : 0)
        uiPalette: root.uiPalette
        text: I18n.i18nc("@action:button", "Close")
        icon.name: "window-close"
        onClicked: root.closeViewer()
    }
}
