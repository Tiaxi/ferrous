// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot
    // Asked because Ferrous is quitting, not because the editor is closing.
    property bool forQuit: false

    // The editor answers these. Saving keeps this dialog open until the save
    // has finished, so its buttons stay disabled meanwhile.
    signal saveRequested()
    signal discardRequested()
    signal keepEditingRequested()

    objectName: "tagEditorCloseConfirmDialog"
    parent: Overlay.overlay
    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: 400
    title: "Unsaved Changes"
    standardButtons: Dialog.NoButton
    // Escape keeps editing, except while a save is running.
    closePolicy: root.tagEditorApi.saving ? Popup.NoAutoClose : Popup.CloseOnEscape

    onRejected: root.keepEditingRequested()

    background: Rectangle {
        radius: 6
        color: root.uiPalette.uiPaneColor
        border.color: root.uiPalette.uiBorderColor
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            objectName: "tagEditorCloseConfirmMessage"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.forQuit
                ? "Save tag changes before quitting Ferrous?"
                : "Save changes before closing the tag editor?"
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.uiPalette.uiMutedTextColor
            text: root.forQuit
                ? "Save writes the current edits and quits. Discard drops every unsaved change and quits. Cancel keeps Ferrous open."
                : "Save writes the current edits. Discard closes the dialog and drops every unsaved change."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                objectName: "tagEditorCloseConfirmKeepEditingButton"
                text: root.forQuit ? "Cancel" : "Keep Editing"
                enabled: !root.tagEditorApi.saving
                onClicked: root.reject()
            }
            Button {
                objectName: "tagEditorCloseConfirmDiscardButton"
                text: "Discard"
                enabled: !root.tagEditorApi.saving
                onClicked: {
                    root.close()
                    root.discardRequested()
                }
            }
            Button {
                objectName: "tagEditorCloseConfirmSaveButton"
                text: root.tagEditorApi.saving ? "Saving..." : "Save"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.tagEditorApi.dirty
                highlighted: true
                onClicked: root.saveRequested()
            }
        }
    }
}
