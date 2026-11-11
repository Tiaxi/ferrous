// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// Shown while quitting waits for a tag save or rename to finish.
Components.CompactDialogWindow {
    id: root

    required property var tagEditorApi
    // Set once the wait has gone on long enough to offer quitting without it.
    property bool offerChoice: false
    readonly property var files: root.tagEditorApi.operationFiles || []
    readonly property int shownFileCount: 5

    signal keepWaitingRequested()
    signal quitAnywayRequested()
    // Escape keeps Ferrous open and stops quitting.
    signal stopQuittingRequested()

    objectName: "tagEditorQuitWaitDialog"
    title: I18n.i18nc("@title:window", "Finishing Tag Changes")
    defaultWidth: Math.max(Kirigami.Units.gridUnit * 26, root.minimumWidth)

    onRejected: root.stopQuittingRequested()

    footer: Components.DialogFooter {

        id: dialogButtons
        primary: tagEditorQuitWaitKeepWaitingButton
        visible: root.offerChoice

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
