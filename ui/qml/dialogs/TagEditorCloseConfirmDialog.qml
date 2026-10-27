// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import FerrousUi 1.0

Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot
    // What the edits would be dropped for: "close" the editor, "quit"
    // Ferrous, or "reload" the files.
    property string purpose: "close"
    readonly property bool forQuit: root.purpose === "quit"
    readonly property bool forReload: root.purpose === "reload"

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
    title: I18n.i18nc("@title:window", "Unsaved Changes")
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
                ? I18n.i18nc("@info", "Save tag changes before quitting Ferrous?")
                : (root.forReload
                    ? I18n.i18nc("@info", "Discard unsaved changes and reload the tags from the files?")
                    : I18n.i18nc("@info", "Save changes before closing the tag editor?"))
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.uiPalette.uiMutedTextColor
            text: root.forQuit
                ? I18n.i18nc("@info", "Save writes the current edits and quits. "
                    + "Discard drops every unsaved change and quits. Cancel keeps Ferrous open.")
                : (root.forReload
                    ? I18n.i18nc("@info", "Reloading drops every unsaved change. Cancel keeps editing.")
                    : I18n.i18nc("@info",
                        "Save writes the current edits. Discard closes the dialog and drops every unsaved change."))
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                objectName: "tagEditorCloseConfirmKeepEditingButton"
                text: root.forQuit || root.forReload
                    ? I18n.i18nc("@action:button", "Cancel")
                    : I18n.i18nc("@action:button", "Keep Editing")
                enabled: !root.tagEditorApi.saving
                onClicked: root.reject()
            }
            Button {
                objectName: "tagEditorCloseConfirmDiscardButton"
                text: root.forReload
                    ? I18n.i18nc("@action:button", "Discard and Reload")
                    : I18n.i18nc("@action:button", "Discard")
                enabled: !root.tagEditorApi.saving
                onClicked: {
                    root.close()
                    root.discardRequested()
                }
            }
            Button {
                objectName: "tagEditorCloseConfirmSaveButton"
                visible: !root.forReload
                text: root.tagEditorApi.saving
                    ? I18n.i18nc("@action:button", "Saving...")
                    : I18n.i18nc("@action:button", "Save")
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.tagEditorApi.dirty
                highlighted: true
                onClicked: root.saveRequested()
            }
        }
    }
}
