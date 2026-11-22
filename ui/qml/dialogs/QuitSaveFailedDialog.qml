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
    // At least as wide as the heading, which is one line in any font.
    contentWidth: quitSaveFailedHeading.implicitWidth

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
    // of its own below, as the confirmations name their file:
    // the error is the system's, such as "Permission denied (os error
    // 13)", and reads as a detail, not as the heading.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2

        Label {
            id: quitSaveFailedHeading
            objectName: "quitSaveFailedHeading"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            // Short enough for one line: what it can be is in the text
            // below (the list in it left "saved." alone on a
            // second line). The lead, semibold, as every dialog's first
            // sentence is (the three lines read as one grey block). It says
            // what happened, not again what the title says ("could not be
            // saved" was there twice).
            font.weight: Font.DemiBold
            textFormat: Text.PlainText
            text: I18n.i18nc("@info", "Some recent changes were not saved.")
        }

        // A path in it breaks between its folders, and the error's code
        // stays whole ("13)" was alone on a line).
        Components.MessageLabel {
            objectName: "quitSaveFailedMessage"
            Layout.fillWidth: true
            visible: root.message.length > 0
            color: root.uiPalette.textSecondary
            message: root.message
        }
    }

    Label {
        objectName: "quitSaveFailedDetails"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: root.uiPalette.textSecondary
        // It names what it speaks of, rather than "They", which could be
        // read as the error above it. It suggests no cure: the error above
        // says the cause, which is not always a full disk.
        textFormat: Text.PlainText
        text: I18n.i18nc("@info", "Unsaved changes can include settings, queue changes and Last.fm scrobbles. "
            + "Retry saves again. Quit Anyway loses them.")
    }
}
