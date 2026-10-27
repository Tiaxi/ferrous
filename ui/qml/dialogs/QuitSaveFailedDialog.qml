// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import FerrousUi 1.0

// Shown when the settings, the queue, or Last.fm's scrobbles could not be
// saved before quitting.
Dialog {
    id: root

    required property var uiPalette
    required property var windowRoot
    property string message: ""

    signal retryRequested()
    signal quitRequested()

    objectName: "quitSaveFailedDialog"
    parent: Overlay.overlay
    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: 440
    title: I18n.i18nc("@title:window", "Couldn't Save Before Quitting")
    standardButtons: Dialog.NoButton
    // Escape keeps Ferrous open.
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        radius: 6
        color: root.uiPalette.uiPaneColor
        border.color: root.uiPalette.uiBorderColor
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            objectName: "quitSaveFailedMessage"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.message
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.uiPalette.uiMutedTextColor
            text: I18n.i18nc("@info", "Retry saves again, for example after freeing disk space. Quit Anyway "
                + "closes Ferrous, and the settings, queue changes, and Last.fm scrobbles "
                + "that could not be saved are lost.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                objectName: "quitSaveFailedQuitButton"
                text: I18n.i18nc("@action:button", "Quit Anyway")
                onClicked: {
                    root.close()
                    root.quitRequested()
                }
            }
            Button {
                objectName: "quitSaveFailedRetryButton"
                text: I18n.i18nc("@action:button", "Retry")
                highlighted: true
                onClicked: {
                    root.close()
                    root.retryRequested()
                }
            }
        }
    }
}
