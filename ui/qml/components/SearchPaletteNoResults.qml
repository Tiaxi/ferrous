// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import FerrousUi 1.0
import "." as Components

// When a search palette (SearchPalette) found nothing: what was searched,
// and a hint under it unless the palette shows something else there.
Item {
    id: root
    required property Components.SearchPalette searchPalette
    property string titleText: ""
    property bool hintShown: true
    objectName: root.searchPalette.namePrefix + "NoResults"
    visible: root.searchPalette.noResults
    y: root.searchPalette.messageGap
    width: parent.width
    height: root.hintShown ? root.searchPalette.resultHeight + root.searchPalette.hintHeight
                           : root.searchPalette.resultHeight
    Text {
        objectName: root.searchPalette.namePrefix + "NoResultsTitle"
        x: root.searchPalette.titleX
        width: parent.width - x - root.searchPalette.sideInset
        height: root.searchPalette.resultHeight
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        textFormat: Text.PlainText
        text: root.titleText
        font.family: root.searchPalette.uiPalette.baseFont.family
        font.pointSize: root.searchPalette.uiPalette.basePointSize
        color: root.searchPalette.uiPalette.text
    }
    Text {
        objectName: root.searchPalette.namePrefix + "NoResultsHint"
        visible: root.hintShown
        x: root.searchPalette.titleX
        // Drawn up into the title row's padding, which is 12 px at every
        // font, so this stays 6 px.
        y: root.searchPalette.resultHeight - 6
        width: parent.width - x - root.searchPalette.sideInset
        height: root.searchPalette.hintHeight
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        textFormat: Text.PlainText
        text: I18n.i18nc("@info", "Check the spelling, or try fewer words.")
        font.family: root.searchPalette.uiPalette.baseFont.family
        font.pointSize: root.searchPalette.uiPalette.smallPointSize
        color: root.searchPalette.uiPalette.textSecondary
    }
}
