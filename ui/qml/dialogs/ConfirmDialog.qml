// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/KeyUtils.js" as KeyUtils

// Asks before an action that cannot be undone. Escape and Cancel keep
// things as they are; only the confirm button sends `confirmed`, once each
// time the dialog shows: its buttons take no click as it fades out.
Dialog {
    id: root

    required property var uiPalette
    required property var windowRoot
    property string message: ""
    property string details: ""
    property string confirmText: I18n.i18nc("@action:button", "OK")

    signal confirmed()

    parent: Overlay.overlay
    modal: true
    x: Math.round(((parent ? parent.width : root.windowRoot.width) - width) / 2)
    y: Math.round(((parent ? parent.height : root.windowRoot.height) - height) / 2)
    width: Math.min(Kirigami.Units.gridUnit * 25, root.windowRoot.width - 2 * Kirigami.Units.gridUnit)
    closePolicy: Popup.CloseOnEscape

    onAccepted: root.confirmed()

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

    footer: Components.DialogButtons {

        id: dialogButtons
        enabled: !closingState.closing
        primary: confirmDialogConfirmButton
        Button {
            id: confirmDialogConfirmButton
            objectName: "confirmDialogConfirmButton"
            text: root.confirmText
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            objectName: "confirmDialogCancelButton"
            text: I18n.i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    contentItem: ColumnLayout {
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, KeyUtils.popupTakesKeys(root)) }

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
            color: root.uiPalette.textSecondary
            text: root.details
        }
    }
}
