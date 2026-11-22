// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components
import "../logic/ColorUtils.js" as ColorUtils

// The controls of a viewer, as flat 24 px tool buttons 4 px apart on the
// window's colours, or with `plotColors` on the plot's: "Not following" while
// the visualization's view is moved away from the playhead, a quiet zoom readout
// and Reset's glyph while the view is zoomed, a button to Recent Issues while there
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
    // Whether the view does not follow the playhead, as the plot's zoom
    // plate says, and Follow Playhead's shortcut for that.
    property bool noticeShown: false
    property string followShortcut: ""
    property string resetToolTip: I18n.i18nc("@info:tooltip", "Reset Zoom")
    property var errorController: null
    // The readout's size: the small font's, or a title strip's own.
    property real readoutPointSize: root.uiPalette.smallPointSize
    property string closeButtonName: "viewerCloseButton"
    // The viewer's own button before Close, if it has one.
    default property alias viewerControls: viewerControlsRow.data
    // On the windowed viewer's corner plate, the zoom is set off from the
    // viewer's controls by a 1 px divider with the controls' 4 px either
    // side, as on the visualization viewer's.
    property bool dividers: false
    // The viewer's own controls set off from Close by a 1 px divider, with
    // the controls' 4 px either side: the visualization viewer's mode switch
    // in its title strip, a choice of two, beside the button that closes it.
    property bool closeDivided: false
    // Close, unless the viewer has none here, as with its strip turned off.
    property bool closeShown: true
    // What a plate under the controls has around them: the readout's text
    // and the dividers are 6 px in from a plate's edge, as on the plot's.
    property real platePadding: 2

    readonly property bool zoomed: Math.abs(root.zoomLevel - 1.0) > 0.001
    readonly property int unseenIssueCount: root.errorController ? root.errorController.unseenCount : 0
    readonly property real spacing: 4
    // Which items take room, whatever the auto-hide does.
    readonly property bool readoutShown: root.zoomed
    readonly property bool resetShown: root.zoomed && root.resetView !== null
    readonly property bool issuesShown: root.unseenIssueCount > 0
    readonly property bool viewerControlsShown: viewerControlsRow.implicitWidth > 0

    implicitWidth: root.closeShown ? closeButton.x + closeButton.width
                                   : viewerControlsRow.x + viewerControlsRow.implicitWidth
    implicitHeight: root.uiPalette.toolButtonSize

    // First, so that it grows the controls to the left.
    Components.FollowNotice {
        id: notice
        objectName: "viewerFollowNotice"
        x: 0
        visible: root.noticeShown
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        pointSize: root.readoutPointSize
        shortcut: root.followShortcut
        divided: root.readoutShown
        inset: root._dividerInset
    }

    // Quiet: only while the view is zoomed. On the album art viewer's
    // corner plate, 6 px in from its edge, as the plot's zoom readout is.
    Components.ZoomReadout {
        id: readout
        objectName: "viewerZoomReadout"
        // Its text 6 px from the notice's divider, as from a plate's edge.
        x: root.noticeShown ? notice.implicitWidth + root.platePadding : 0
        leftPadding: 6 - root.platePadding
        visible: root.readoutShown
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        zoomLevel: root.zoomLevel
        pointSize: root.readoutPointSize
        rightPadding: 0
    }

    // The plot's Reset Zoom (PlotZoomPlate), its glyph as far from the
    // readout (as the word "Reset" in the strip, it was another control
    // than the glyph in the window).
    Components.ViewerToolButton {
        id: resetButton
        objectName: "viewerResetButton"
        x: readout.x + (root.readoutShown ? readout.width + 2 : 0)
        visible: root.resetShown
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        text: root.resetToolTip
        icon.name: "zoom-original"
        onClicked: root.resetView()
    }

    // Where the view's part, the notice and the zoom, ends, and what
    // follows it starts.
    readonly property real _zoomEnd: root.resetShown ? resetButton.x + resetButton.width
        : root.readoutShown ? readout.x + readout.width
        : notice.implicitWidth
    readonly property bool _viewShown: root.readoutShown || root.noticeShown
    readonly property bool _zoomDivided: root.dividers && root._viewShown
    readonly property real _dividerInset: 6 - root.platePadding
    readonly property real _afterZoom: !root._viewShown ? 0
        : root._zoomEnd + root.spacing + (root._zoomDivided ? 1 + root.spacing : 0)

    Rectangle {
        objectName: "viewerControlsDivider"
        visible: root._zoomDivided
        x: root._zoomEnd + root.spacing
        y: root._dividerInset
        width: 1
        height: Math.max(0, root.height - 2 * root._dividerInset)
        color: root.plotColors ? root.plotColors.plotPlateEdge : root.uiPalette.seam
    }

    Components.ViewerToolButton {
        id: issuesButton
        objectName: "viewerIssuesButton"
        x: root._afterZoom
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

    readonly property bool _closeDivided: root.closeShown && root.closeDivided && root.viewerControlsShown

    Rectangle {
        objectName: "viewerControlsCloseDivider"
        visible: root._closeDivided
        x: viewerControlsRow.x + viewerControlsRow.implicitWidth + root.spacing
        y: root._dividerInset
        width: 1
        height: Math.max(0, root.height - 2 * root._dividerInset)
        color: root.plotColors ? root.plotColors.plotPlateEdge : root.uiPalette.seam
    }

    Components.ViewerToolButton {
        id: closeButton
        objectName: root.closeButtonName
        visible: root.closeShown
        x: viewerControlsRow.x + (root.viewerControlsShown ? viewerControlsRow.implicitWidth + root.spacing : 0)
            + (root._closeDivided ? 1 + root.spacing : 0)
        uiPalette: root.uiPalette
        plotColors: root.plotColors
        text: I18n.i18nc("@action:button", "Close")
        icon.name: "window-close"
        onClicked: root.closeViewer()
    }
}
