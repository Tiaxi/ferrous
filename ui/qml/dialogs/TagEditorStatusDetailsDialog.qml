// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// The whole of the tag editor's last status, such as each file a save
// could not write, in a window of its own over the editor.
Components.DialogWindow {
    id: root

    required property var tagEditorApi

    objectName: "tagEditorStatusDetailsDialog"
    title: I18n.i18nc("@title:window", "Tag Editor Details")
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 12
    defaultWidth: Kirigami.Units.gridUnit * 36
    defaultHeight: Kirigami.Units.gridUnit * 29

    onAboutToShow: statusDetailsCloseButton.forceActiveFocus(Qt.PopupFocusReason)

    footer: Components.DialogButtons {
        primary: statusDetailsCloseButton
        Button {
            id: statusDetailsCloseButton
            objectName: "tagEditorStatusDetailsCloseButton"
            text: I18n.i18nc("@action:button", "Close")
            icon.name: "window-close"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    ScrollView {
        anchors.fill: parent
        anchors.leftMargin: Kirigami.Units.gridUnit
        anchors.rightMargin: Kirigami.Units.gridUnit
        anchors.topMargin: Kirigami.Units.gridUnit
        clip: true

        TextArea {
            objectName: "tagEditorStatusDetailsText"
            readOnly: true
            wrapMode: TextEdit.Wrap
            text: root.tagEditorApi.statusDetails
            selectByMouse: true
            background: null
        }
    }
}
