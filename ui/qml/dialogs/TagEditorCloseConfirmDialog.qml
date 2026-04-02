// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot
    required property var closeEditor
    required property var saveAndClose

    parent: Overlay.overlay
    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: 400
    title: "Unsaved Changes"
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
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Save changes before closing the tag editor?"
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.uiPalette.uiMutedTextColor
            text: "Save writes the current edits. Discard closes the dialog and drops every unsaved change."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                text: "Keep Editing"
                onClicked: root.close()
            }
            Button {
                text: "Discard"
                onClicked: {
                    root.close()
                    root.closeEditor()
                }
            }
            Button {
                text: "Save"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.tagEditorApi.dirty
                highlighted: true
                onClicked: root.saveAndClose()
            }
        }
    }
}
