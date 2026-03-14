import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../components" as Components

Dialog {
    id: root

    required property var uiBridge
    required property var palette
    required property var windowRoot
    required property int popupTransitionMs
    required property string dialogMode
    required property string pathValue
    required property string nameValue
    signal dismissed()

    modal: true
    title: root.dialogMode === "rename" ? "Rename Library Root" : "Add Library Root"
    standardButtons: Dialog.Ok | Dialog.Cancel
    width: Math.min(560, root.windowRoot.width - 80)
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

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: "Path"
            color: root.palette.uiMutedTextColor
        }

        TextField {
            Layout.fillWidth: true
            readOnly: true
            text: root.pathValue || ""
            selectByMouse: true
        }

        Label {
            Layout.fillWidth: true
            text: "Custom Name (optional)"
            color: root.palette.uiMutedTextColor
        }

        TextField {
            id: libraryRootNameField
            Layout.fillWidth: true
            text: root.nameValue || ""
            placeholderText: "Leave blank to use the path"
            selectByMouse: true
            onAccepted: root.accept()
        }
    }
}
