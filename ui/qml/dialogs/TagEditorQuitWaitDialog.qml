// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/KeyUtils.js" as KeyUtils

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
    width: Math.min(Kirigami.Units.gridUnit * 26, root.windowRoot.width - 2 * Kirigami.Units.gridUnit)
    title: I18n.i18nc("@title:window", "Finishing Tag Changes")
    closePolicy: Popup.CloseOnEscape

    onRejected: root.stopQuittingRequested()

    // Quit Anyway closes it, and its buttons take no other answer as it
    // fades out.
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

    // The dialogs' one heading style, in place of the style's larger one.
    header: Components.DialogHeader {
        dialog: root
        uiPalette: root.uiPalette
    }

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

    footer: Components.DialogFooter {

        id: dialogButtons
        primary: tagEditorQuitWaitKeepWaitingButton
        visible: root.offerChoice
        enabled: !closingState.closing

        Button {
            id: tagEditorQuitWaitKeepWaitingButton
            objectName: "tagEditorQuitWaitKeepWaitingButton"
            text: I18n.i18nc("@action:button", "Keep Waiting")
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: root.keepWaitingRequested()
        }
        Button {
            objectName: "tagEditorQuitWaitQuitButton"
            text: I18n.i18nc("@action:button", "Quit Anyway")
            icon.name: "application-exit"
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: root.quitAnywayRequested()
        }
    }

    contentItem: ColumnLayout {
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, KeyUtils.popupTakesKeys(root)) }

        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            BusyIndicator {
                running: root.visible
                implicitWidth: Kirigami.Units.iconSizes.smallMedium
                implicitHeight: Kirigami.Units.iconSizes.smallMedium
            }

            Label {
                objectName: "tagEditorQuitWaitStatus"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: I18n.i18ncp("@info:status", "Ferrous quits once the tag editor has written %1 file.",
                    "Ferrous quits once the tag editor has written %1 files.", root.files.length)
            }
        }

        Label {
            objectName: "tagEditorQuitWaitFiles"
            Layout.fillWidth: true
            visible: root.files.length > 0
            wrapMode: Text.WordWrap
            color: root.uiPalette.textSecondary
            text: root.files.slice(0, root.shownFileCount).join("\n")
                + (root.files.length > root.shownFileCount
                    ? "\n" + I18n.i18ncp("@info more files than the list shows", "and %1 more", "and %1 more",
                        root.files.length - root.shownFileCount)
                    : "")
        }

        Label {
            objectName: "tagEditorQuitWaitMessage"
            Layout.fillWidth: true
            visible: root.offerChoice
            wrapMode: Text.WordWrap
            text: I18n.i18nc("@info", "This is taking longer than it should, for example on an unresponsive "
                + "network share. Quit Anyway stops waiting, and the tag changes not yet "
                + "written may be lost. Each file keeps its old tags or gets its new ones; "
                + "none is left half-written.")
        }
    }
}
