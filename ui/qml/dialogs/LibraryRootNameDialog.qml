// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

Dialog {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var windowRoot
    required property int popupTransitionMs
    required property string dialogMode
    required property string pathValue
    required property string nameValue
    signal dismissed()

    modal: true
    title: root.dialogMode === "rename"
        ? I18n.i18nc("@title:window", "Rename Library Folder")
        : I18n.i18nc("@title:window", "Add Library Folder")
    width: Math.min(Kirigami.Units.gridUnit * 30, root.windowRoot.width - 2 * Kirigami.Units.gridUnit)
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }

    onOpened: {
        libraryRootNameField.forceActiveFocus()
        libraryRootNameField.selectAll()
    }

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

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

    footer: Components.DialogButtons {
        primary: libraryRootNameAcceptButton
        Button {
            id: libraryRootNameAcceptButton
            objectName: "libraryRootNameAcceptButton"
            text: root.dialogMode === "rename"
                ? I18n.i18nc("@action:button", "Rename")
                : I18n.i18nc("@action:button", "Add Folder")
            icon.name: root.dialogMode === "rename" ? "edit-rename" : "folder-add"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            objectName: "libraryRootNameCancelButton"
            text: I18n.i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    contentItem: Kirigami.FormLayout {
        TextField {
            objectName: "libraryRootPathField"
            Kirigami.FormData.label: I18n.i18nc("@label:textbox", "Folder:")
            Layout.fillWidth: true
            Layout.minimumWidth: Kirigami.Units.gridUnit * 18
            readOnly: true
            text: root.pathValue || ""
            selectByMouse: true
        }

        TextField {
            id: libraryRootNameField
            objectName: "libraryRootNameField"
            Kirigami.FormData.label: I18n.i18nc("@label:textbox", "Name:")
            Layout.fillWidth: true
            text: root.nameValue || ""
            placeholderText: I18n.i18nc("@info:placeholder", "The folder\u2019s path")
            selectByMouse: true
            onAccepted: root.accept()
        }

        Components.FormHelpText {
            uiPalette: root.uiPalette
            text: I18n.i18nc("@info", "Optional. The library shows the folder by this name.")
        }
    }
}
