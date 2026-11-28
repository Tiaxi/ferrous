// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import "." as Components

// A run of a search result's title (SearchPalette.titleRuns): the matches
// semibold. A row has one for each of the palette's maxTitleRuns, plain
// items rather than a Repeater, which would make each row slower to
// create, and sets their colour.
Text {
    id: root
    required property Components.SearchPalette searchPalette
    property var run: null
    objectName: root.searchPalette.namePrefix + "ResultTitle"
    visible: !!root.run && root.run.width > 0
    x: root.searchPalette.titleX + (root.run ? root.run.x : 0)
    width: root.run ? root.run.width : 0
    height: root.searchPalette.resultHeight
    verticalAlignment: Text.AlignVCenter
    text: root.run ? root.run.text : ""
    textFormat: Text.PlainText
    elide: Text.ElideRight
    font.family: root.searchPalette.uiPalette.baseFont.family
    font.pointSize: root.searchPalette.uiPalette.basePointSize
    font.weight: root.run && root.run.match ? Font.DemiBold : Font.Normal
}
