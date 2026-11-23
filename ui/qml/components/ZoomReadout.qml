// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import FerrousUi 1.0

// The quiet zoom readout, "Zoom 244 %", of the viewers and the main pane:
// in the small font, or in a title strip at the strip's size, with tabular
// figures, in the plot's secondary text colour, or without `plotColors` the
// window's. It is as wide as its text, so that what is beside it keeps its
// spacing at every digit count: the plates it is on are anchored at their
// right, where Reset stays, and grow to the left. (As wide as its widest
// four-digit value, it left 18 px between "Not following"'s divider and a
// three-digit zoom, against the 6 px on the divider's other side.)
Label {
    id: root

    required property var uiPalette
    // The plot's colours (UiPalette's PlotColors), or null for the window's.
    property var plotColors: null
    // The view's zoom, 1 at the default.
    property real zoomLevel: 1.0
    property real pointSize: root.uiPalette.smallPointSize

    height: root.uiPalette.toolButtonSize
    verticalAlignment: Text.AlignVCenter
    textFormat: Text.PlainText
    text: I18n.i18nc("@info:status zoom of the view in percent", "Zoom %1%",
        Math.round(root.zoomLevel * 100))
    font.family: root.uiPalette.baseFont.family
    font.pointSize: root.pointSize
    font.features: root.uiPalette.tabularFigures
    color: root.plotColors ? root.plotColors.plotTextSecondary : root.uiPalette.textSecondary
}
