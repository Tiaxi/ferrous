// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/KeyUtils.js" as KeyUtils

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
    width: Math.min(Kirigami.Units.gridUnit * 25, root.windowRoot.width - 2 * Kirigami.Units.gridUnit)
    title: I18n.i18nc("@title:window", "Unsaved Changes")
    // Escape keeps editing, except while a save is running.
    closePolicy: root.tagEditorApi.saving ? Popup.NoAutoClose : Popup.CloseOnEscape

    onRejected: root.keepEditingRequested()

    // An answer closes it, and its buttons take no other as it fades out.
    Components.PopupClosing {
        id: closingState
        popup: root
    }

    // The keyboard in the content, whose Enter presses the default button,
    // from the moment the dialog shows.
    Components.PopupInitialFocus {
        popup: root
        item: root.contentItem
    }

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

    // Save, Discard, Cancel.
    footer: Components.DialogFooter {
        id: dialogButtons
        enabled: !closingState.closing
        // Save; or, on the reload question, which has no Save, Cancel: the
        // default button never drops the edits.
        primary: root.forReload ? keepEditingButton : saveButton
        Button {
            id: saveButton
            objectName: "tagEditorCloseConfirmSaveButton"
            visible: !root.forReload
            text: root.tagEditorApi.saving
                ? I18n.i18nc("@action:button", "Saving\u2026")
                : I18n.i18nc("@action:button", "Save")
            icon.name: "document-save"
            enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.tagEditorApi.dirty
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: root.saveRequested()
        }
        Button {
            id: discardButton
            objectName: "tagEditorCloseConfirmDiscardButton"
            text: root.forReload
                ? I18n.i18nc("@action:button", "Discard and Reload")
                : I18n.i18nc("@action:button", "Discard")
            icon.name: "edit-delete-remove"
            enabled: !root.tagEditorApi.saving
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                root.close()
                root.discardRequested()
            }
        }
        Button {
            id: keepEditingButton
            objectName: "tagEditorCloseConfirmKeepEditingButton"
            text: root.forQuit || root.forReload
                ? I18n.i18nc("@action:button", "Cancel")
                : I18n.i18nc("@action:button", "Keep Editing")
            icon.name: "dialog-cancel"
            enabled: !root.tagEditorApi.saving
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    contentItem: ColumnLayout {
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, KeyUtils.popupTakesKeys(root)) }

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
            color: root.uiPalette.textSecondary
            text: root.forQuit
                ? I18n.i18nc("@info", "Save writes the current edits and quits. "
                    + "Discard drops every unsaved change and quits. Cancel keeps Ferrous open.")
                : (root.forReload
                    ? I18n.i18nc("@info", "Reloading drops every unsaved change. Cancel keeps editing.")
                    : I18n.i18nc("@info",
                        "Save writes the current edits. Discard closes the dialog and drops every unsaved change."))
        }
    }
}
