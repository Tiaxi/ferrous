// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import FerrousUi 1.0
import "../components" as Components

// Shown when the settings, the queue, or Last.fm's scrobbles could not be
// saved before quitting.
Components.CompactDialogWindow {
    id: root

    property string message: ""

    signal retryRequested()
    signal quitRequested()

    objectName: "quitSaveFailedDialog"
    title: I18n.i18nc("@title:window", "Couldn\u2019t Save Before Quitting")
    // Escape and the title bar's close button keep Ferrous open.

    footer: Components.DialogFooter {
        id: dialogButtons
        primary: quitSaveFailedRetryButton
        Button {
            id: quitSaveFailedRetryButton
            objectName: "quitSaveFailedRetryButton"
            text: I18n.i18nc("@action:button", "Retry")
            icon.name: "view-refresh"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: {
                // Hidden at once: a second click finds no button.
                root.dismiss()
                root.retryRequested()
            }
        }
        Button {
            objectName: "quitSaveFailedQuitButton"
            text: I18n.i18nc("@action:button", "Quit Anyway")
            icon.name: "application-exit"
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                root.dismiss()
                root.quitRequested()
            }
        }
    }

    // What happened, in words, with the error that stopped it on a line
    // of its own below, as the confirmations name their file (#373 H37):
    // the error is the system's, such as "Permission denied (os error
    // 13)", and reads as a detail, not as the heading.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2

        Label {
            objectName: "quitSaveFailedHeading"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: I18n.i18nc("@info", "Some settings, queue changes, or Last.fm scrobbles could not be saved.")
        }

        Label {
            objectName: "quitSaveFailedMessage"
            Layout.fillWidth: true
            visible: root.message.length > 0
            // A path in it may have no space to break at.
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: root.uiPalette.textSecondary
            text: root.message
        }
    }

    Label {
        objectName: "quitSaveFailedDetails"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: root.uiPalette.textSecondary
        text: I18n.i18nc("@info", "Retry saves again, for example after freeing disk space. Quit Anyway "
            + "closes Ferrous, and what could not be saved is lost.")
    }
}
