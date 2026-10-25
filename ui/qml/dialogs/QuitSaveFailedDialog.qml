// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// Shown when the settings and the queue could not be saved before quitting.
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
    title: "Couldn't Save Before Quitting"
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
            text: "Retry saves again, for example after freeing disk space. Quit Anyway "
                + "closes Ferrous, and the settings and queue changes that could not be "
                + "saved are lost."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                objectName: "quitSaveFailedQuitButton"
                text: "Quit Anyway"
                onClicked: {
                    root.close()
                    root.quitRequested()
                }
            }
            Button {
                objectName: "quitSaveFailedRetryButton"
                text: "Retry"
                highlighted: true
                onClicked: {
                    root.close()
                    root.retryRequested()
                }
            }
        }
    }
}
