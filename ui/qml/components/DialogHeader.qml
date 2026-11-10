// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

// The title row of a dialog drawn in a window, such as a question over
// the window it asks for: its title in the one heading style of the
// dialogs, Configure's page titles' 15 px semibold with the default font,
// and a close button, which rejects it. A dialog in a window of its own
// has KWin's title bar instead.
Control {
    id: root

    required property var dialog
    property var uiPalette: null
    property bool closeEnabled: true

    visible: root.dialog.title.length > 0
    // The title on the content's inset, the close button near the corner.
    leftPadding: Kirigami.Units.gridUnit
    rightPadding: Kirigami.Units.largeSpacing
    topPadding: Kirigami.Units.largeSpacing
    // The dialog's content inset follows.
    bottomPadding: 0

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Label {
            objectName: "dialogHeaderTitle"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: root.dialog.title
            textFormat: Text.PlainText
            // 15 px with the default 10 pt font, as Configure's page titles.
            font.pointSize: (root.uiPalette ? root.uiPalette.basePointSize : Kirigami.Theme.defaultFont.pointSize)
                * 1.125
            font.weight: Font.DemiBold
            color: root.uiPalette ? root.uiPalette.text : Kirigami.Theme.textColor
            elide: Text.ElideRight
            Accessible.role: Accessible.Heading
        }

        ToolButton {
            objectName: "dialogHeaderCloseButton"
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            enabled: root.closeEnabled
            icon.name: hovered ? "window-close" : "window-close-symbolic"
            text: I18n.i18nc("@action:button close dialog", "Close")
            display: AbstractButton.IconOnly
            ToolTip.visible: hovered || visualFocus
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: text
            onClicked: root.dialog.reject()
        }
    }
}
