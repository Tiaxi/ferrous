// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

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
    width: Math.min(Kirigami.Units.gridUnit * 25, root.windowRoot.width - 2 * Kirigami.Units.gridUnit)
    title: I18n.i18nc("@title:window", "Couldn\u2019t Save Before Quitting")
    // Escape keeps Ferrous open.
    closePolicy: Popup.CloseOnEscape

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

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
                root.close()
                root.retryRequested()
            }
        }
        Button {
            objectName: "quitSaveFailedQuitButton"
            text: I18n.i18nc("@action:button", "Quit Anyway")
            icon.name: "application-exit"
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                root.close()
                root.quitRequested()
            }
        }
    }

    contentItem: ColumnLayout {
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem) }

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
            color: root.uiPalette.textSecondary
            text: I18n.i18nc("@info", "Retry saves again, for example after freeing disk space. Quit Anyway "
                + "closes Ferrous, and the settings, queue changes, and Last.fm scrobbles "
                + "that could not be saved are lost.")
        }
    }
}
