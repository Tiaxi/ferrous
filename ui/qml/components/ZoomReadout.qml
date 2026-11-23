// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import FerrousUi 1.0

// The quiet zoom readout, "Zoom 244 %", of the viewers and the main pane:
// in the small font, or in a title strip at the strip's size, with tabular
// figures, in the plot's secondary text colour, or without `plotColors` the
// window's. It is as wide as its widest four-digit value, its text to the
// right, so that it and what is left of it keep their places as the
// digits' count changes in a wheel zoom.
Label {
    id: root

    required property var uiPalette
    // The plot's colours (UiPalette's PlotColors), or null for the window's.
    property var plotColors: null
    // The view's zoom, 1 at the default.
    property real zoomLevel: 1.0
    property real pointSize: root.uiPalette.smallPointSize

    function readout(percent) {
        return I18n.i18nc("@info:status zoom of the view in percent", "Zoom %1%", percent)
    }

    width: Math.max(root.implicitWidth,
        Math.ceil(widestReadout.advanceWidth) + root.leftPadding + root.rightPadding)
    height: root.uiPalette.toolButtonSize
    horizontalAlignment: Text.AlignRight
    verticalAlignment: Text.AlignVCenter
    textFormat: Text.PlainText
    text: root.readout(Math.round(root.zoomLevel * 100))
    font.family: root.uiPalette.baseFont.family
    font.pointSize: root.pointSize
    font.features: root.uiPalette.tabularFigures
    color: root.plotColors ? root.plotColors.plotTextSecondary : root.uiPalette.textSecondary

    TextMetrics {
        id: widestReadout
        font: root.font
        text: root.readout(8888)
    }
}
