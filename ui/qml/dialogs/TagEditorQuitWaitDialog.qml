// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// Shown while quitting waits for a tag save or rename to finish.
Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot
    // Set once the wait has gone on long enough to offer quitting without it.
    property bool offerChoice: false
    readonly property var files: root.tagEditorApi.operationFiles || []
    readonly property int shownFileCount: 5

    signal keepWaitingRequested()
    signal quitAnywayRequested()
    // Escape keeps Ferrous open and stops quitting.
    signal stopQuittingRequested()

    objectName: "tagEditorQuitWaitDialog"
    parent: Overlay.overlay
    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: 460
    title: "Finishing Tag Changes"
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape

    onRejected: root.stopQuittingRequested()

    background: Rectangle {
        radius: 6
        color: root.uiPalette.uiPaneColor
        border.color: root.uiPalette.uiBorderColor
    }

    contentItem: ColumnLayout {
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            BusyIndicator {
                running: root.visible
                implicitWidth: 20
                implicitHeight: 20
            }

            Label {
                objectName: "tagEditorQuitWaitStatus"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "Ferrous quits once the tag editor has written "
                    + (root.files.length === 1 ? "1 file" : root.files.length + " files") + "."
            }
        }

        Label {
            objectName: "tagEditorQuitWaitFiles"
            Layout.fillWidth: true
            visible: root.files.length > 0
            wrapMode: Text.WordWrap
            color: root.uiPalette.uiMutedTextColor
            text: root.files.slice(0, root.shownFileCount).join("\n")
                + (root.files.length > root.shownFileCount
                    ? "\nand " + (root.files.length - root.shownFileCount) + " more"
                    : "")
        }

        Label {
            objectName: "tagEditorQuitWaitMessage"
            Layout.fillWidth: true
            visible: root.offerChoice
            wrapMode: Text.WordWrap
            text: "This is taking longer than it should, for example on an unresponsive "
                + "network share. Quit Anyway stops waiting, and the tag changes not yet "
                + "written may be lost. Each file keeps its old tags or gets its new ones; "
                + "none is left half-written."
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.offerChoice
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                objectName: "tagEditorQuitWaitQuitButton"
                text: "Quit Anyway"
                onClicked: root.quitAnywayRequested()
            }
            Button {
                objectName: "tagEditorQuitWaitKeepWaitingButton"
                text: "Keep Waiting"
                highlighted: true
                onClicked: root.keepWaitingRequested()
            }
        }
    }
}
