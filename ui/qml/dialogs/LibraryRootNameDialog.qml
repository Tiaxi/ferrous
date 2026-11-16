// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// Adds a library folder, or renames one, by the name the library shows it
// by. A window of its own, over Configure, whose Library page asks for it,
// or over the main window.
Components.CompactDialogWindow {
    id: root

    required property var uiBridge
    required property string dialogMode
    required property string pathValue
    required property string nameValue
    signal dismissed()

    title: root.dialogMode === "rename"
        ? I18n.i18nc("@title:window", "Rename Library Folder")
        : I18n.i18nc("@title:window", "Add Library Folder")
    // As wide as its form, whose path shows a typical folder whole.
    contentWidth: libraryRootNameForm.implicitWidth
    defaultWidth: Math.max(root.minimumWidth, root._neededWidth)
    initialFocusItem: libraryRootNameField

    onAboutToShow: libraryRootNameField.selectAll()

    onAccepted: {
        const resolvedPath = root.pathValue || ""
        const resolvedName = (libraryRootNameField.text || "").trim()
        if (resolvedPath.length > 0) {
            if (root.dialogMode === "rename") {
                root.uiBridge.setLibraryRootName(resolvedPath, resolvedName)
            } else {
                root.uiBridge.addLibraryRoot(resolvedPath, resolvedName)
            }
        }
        root.dismissed()
    }

    onRejected: root.dismissed()

    footer: Components.DialogButtons {
        id: dialogButtons
        primary: libraryRootNameAcceptButton
        Button {
            id: libraryRootNameAcceptButton
            objectName: "libraryRootNameAcceptButton"
            text: root.dialogMode === "rename"
                ? I18n.i18nc("@action:button", "Rename")
                : I18n.i18nc("@action:button", "Add Folder")
            icon.name: root.dialogMode === "rename" ? "edit-rename" : "folder-add"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: root.accept()
        }
        Button {
            objectName: "libraryRootNameCancelButton"
            text: I18n.i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    Kirigami.FormLayout {
        id: libraryRootNameForm
        objectName: "libraryRootNameForm"
        Layout.fillWidth: true

        // The folder is chosen already: its path to read, not to edit.
        Components.PathLabel {
            objectName: "libraryRootPathField"
            Kirigami.FormData.label: I18n.i18nc("@label", "Folder:")
            Layout.fillWidth: true
            // Wide enough for a typical folder's path whole, and for the
            // help under the name on one line; a longer path is elided in
            // the middle (#373 H38). In grid units of the font it is in.
            Layout.preferredWidth: root.uiPalette.gridUnit * 24
            path: root.pathValue || ""
        }

        TextField {
            id: libraryRootNameField
            objectName: "libraryRootNameField"
            Kirigami.FormData.label: I18n.i18nc("@label:textbox", "Name:")
            Layout.fillWidth: true
            text: root.nameValue || ""
            selectByMouse: true
            onAccepted: root.accept()
        }

        Components.FormHelpText {
            uiPalette: root.uiPalette
            text: I18n.i18nc("@info", "Optional. Without a name, the library shows the folder’s path.")
        }
    }
}
