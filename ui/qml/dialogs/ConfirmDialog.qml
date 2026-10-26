// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// Asks before an action that cannot be undone. Escape and Cancel keep
// things as they are; only the confirm button sends `confirmed`.
Dialog {
    id: root

    required property var uiPalette
    required property var windowRoot
    property string message: ""
    property string details: ""
    property string confirmText: "OK"

    signal confirmed()

    parent: Overlay.overlay
    modal: true
    x: Math.round(((parent ? parent.width : root.windowRoot.width) - width) / 2)
    y: Math.round(((parent ? parent.height : root.windowRoot.height) - height) / 2)
    width: 440
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        radius: 6
        color: root.uiPalette.uiPaneColor
        border.color: root.uiPalette.uiBorderColor
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            objectName: "confirmDialogMessage"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.message
        }

        Label {
            objectName: "confirmDialogDetails"
            Layout.fillWidth: true
            visible: root.details.length > 0
            wrapMode: Text.Wrap
            color: root.uiPalette.uiMutedTextColor
            text: root.details
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                objectName: "confirmDialogCancelButton"
                text: "Cancel"
                onClicked: root.reject()
            }
            Button {
                objectName: "confirmDialogConfirmButton"
                text: root.confirmText
                highlighted: true
                onClicked: {
                    root.close()
                    root.confirmed()
                }
            }
        }
    }
}
