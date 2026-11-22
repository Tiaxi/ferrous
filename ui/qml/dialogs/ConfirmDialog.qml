// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import FerrousUi 1.0
import "../components" as Components

// Asks before an action that cannot be undone, in a window of its own
// above the window it asks for. Escape, Cancel and the title bar's close
// button keep things as they are; only the confirm button sends
// `confirmed`, once each time the dialog shows: the window hides as it is
// answered. The confirm button is the default one, which Enter presses,
// unless the action destroys data: then Cancel is, as KDE's guidelines ask,
// and it has the keyboard as the dialog opens, so neither Enter nor Space
// destroys anything until the user moves to the action. The question is
// the dialog's lead, semibold, as every dialog's first sentence is; what
// it acts on and what follows are secondary.
Components.CompactDialogWindow {
    id: root

    property string message: ""
    // The file or folder it acts on, on a line of its own, if it names one.
    property string path: ""
    property string details: ""
    property string confirmText: I18n.i18nc("@action:button", "OK")
    property string confirmIcon: ""
    // Whether the action destroys data, such as a file it deletes or
    // replaces.
    property bool destructive: false

    signal confirmed()

    onAccepted: root.confirmed()

    initialFocusItem: root.destructive ? confirmDialogCancelButton : root.body

    footer: Components.DialogButtons {
        id: dialogButtons
        primary: root.destructive ? confirmDialogCancelButton : confirmDialogConfirmButton
        Button {
            id: confirmDialogConfirmButton
            objectName: "confirmDialogConfirmButton"
            text: root.confirmText
            icon.name: root.confirmIcon
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: root.accept()
        }
        Button {
            id: confirmDialogCancelButton
            objectName: "confirmDialogCancelButton"
            text: I18n.i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2

        Label {
            objectName: "confirmDialogMessage"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.weight: Font.DemiBold
            textFormat: Text.PlainText
            text: root.message
        }

        Components.PathLabel {
            objectName: "confirmDialogPath"
            Layout.fillWidth: true
            visible: root.path.length > 0
            path: root.path
            color: root.uiPalette.textSecondary
        }
    }

    Label {
        objectName: "confirmDialogDetails"
        Layout.fillWidth: true
        visible: root.details.length > 0
        wrapMode: Text.Wrap
        color: root.uiPalette.textSecondary
        textFormat: Text.PlainText
        text: root.details
    }
}
