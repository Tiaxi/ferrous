// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

// The title row of a dialog that decides itself how it closes, such as one
// that asks first about unsaved changes: the style's header, whose close
// button rejects the dialog at once, cannot.
Control {
    id: root

    required property string title
    property bool closeEnabled: true
    property string closeButtonName: "dialogHeaderCloseButton"

    signal closeRequested()

    padding: Kirigami.Units.largeSpacing
    bottomPadding: Kirigami.Units.largeSpacing

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Heading {
            objectName: "dialogHeaderTitle"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            level: 2
            text: root.title
            textFormat: Text.PlainText
            elide: Text.ElideRight
        }

        ToolButton {
            objectName: root.closeButtonName
            Layout.alignment: Qt.AlignRight | Qt.AlignTop
            enabled: root.closeEnabled
            icon.name: hovered ? "window-close" : "window-close-symbolic"
            text: I18n.i18nc("@action:button close dialog", "Close")
            display: AbstractButton.IconOnly
            ToolTip.visible: hovered || visualFocus
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: text
            onClicked: root.closeRequested()
        }
    }
}
