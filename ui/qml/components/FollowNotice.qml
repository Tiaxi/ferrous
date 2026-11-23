// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import FerrousUi 1.0
import org.kde.kirigami 2.20 as Kirigami
import "." as Components

// The quiet "Not following" of a view that was moved away from the
// playhead, before the zoom on the main pane's zoom plate and in the
// viewers' controls: in the zoom readout's size and ink, with a tooltip
// that names the way back, Follow Playhead and its shortcut. It is no
// button (#543). `divided`, the zoom follows it, set off by a 1 px divider
// 6 px from the text.
Item {
    id: root
    objectName: "followNotice"

    required property var uiPalette
    // The plot's colours (UiPalette's PlotColors), or null for the window's.
    property var plotColors: null
    property real pointSize: root.uiPalette.smallPointSize
    // Follow Playhead's shortcut as the user reads it, or "".
    property string shortcut: ""
    property bool divided: false
    // Whether it says so: as the view follows again, its text goes in the
    // frame the view comes back in, while the plate it is on narrows.
    property bool textShown: true
    // The text's and the divider's 6 px from the plate's edge, less what
    // the plate has around them.
    property real inset: 0
    readonly property string toolTipText: root.shortcut.length > 0
        ? I18n.i18nc("@info:tooltip %1 is the shortcut of View › Follow Playhead",
            "Follow Playhead (%1) brings the view back to the playhead", root.shortcut)
        : I18n.i18nc("@info:tooltip View › Follow Playhead is a menu command",
            "Follow Playhead in the View menu brings the view back to the playhead")

    implicitWidth: label.implicitWidth + (root.divided ? 6 + 1 : root.inset)
    implicitHeight: root.uiPalette.toolButtonSize
    width: implicitWidth
    height: implicitHeight

    Accessible.role: Accessible.StaticText
    Accessible.name: label.text
    Accessible.description: root.toolTipText

    Label {
        id: label
        objectName: "followNoticeText"
        visible: root.textShown
        height: root.height
        verticalAlignment: Text.AlignVCenter
        leftPadding: root.inset
        rightPadding: 0
        textFormat: Text.PlainText
        text: I18n.i18nc("@info:status the visualization's view was moved away from the playhead",
            "Not following")
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.pointSize
        color: root.plotColors ? root.plotColors.plotTextSecondary : root.uiPalette.textSecondary
        Accessible.ignored: true
    }

    Rectangle {
        objectName: "followNoticeDivider"
        visible: root.divided && root.textShown
        x: label.width + 6
        y: root.inset
        width: 1
        height: Math.max(0, root.height - 2 * root.inset)
        color: root.plotColors ? root.plotColors.plotPlateEdge : root.uiPalette.seam
    }

    HoverHandler {
        id: noticeHover
    }

    Components.PlainToolTip {
        objectName: "followNoticeToolTip"
        visible: noticeHover.hovered && root.visible && root.textShown
        delay: Kirigami.Units.toolTipDelay
        message: root.toolTipText
    }
}
