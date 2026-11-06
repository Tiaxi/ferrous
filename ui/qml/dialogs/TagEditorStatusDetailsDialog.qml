// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot

    parent: Overlay.overlay
    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: Math.min(root.windowRoot.width - 2 * Kirigami.Units.gridUnit, Kirigami.Units.gridUnit * 36)
    height: Math.min(root.windowRoot.height - 4 * Kirigami.Units.gridUnit, Kirigami.Units.gridUnit * 29)
    title: I18n.i18nc("@title:window", "Tag Editor Details")
    closePolicy: Popup.CloseOnEscape

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

    footer: Components.DialogButtons {
        Button {
            objectName: "tagEditorStatusDetailsCloseButton"
            text: I18n.i18nc("@action:button", "Close")
            icon.name: "window-close"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                readOnly: true
                wrapMode: TextEdit.Wrap
                text: root.tagEditorApi.statusDetails
                selectByMouse: true
                background: null
            }
        }
    }
}
