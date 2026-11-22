// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// Shown while quitting waits for a tag save or rename to finish: what it
// waits for, semibold, beside the spinner, and the files under its text.
// Once it offers to quit anyway, what that costs comes straight after the
// status, and why it offers, shorter, under the files (the cost followed an
// example, in a paragraph of five lines at 24 pt). All of that is in the
// secondary colour, as the explanation of a confirmation is, on the files'
// left edge, so all but the spinner start on one line (the explanation was
// on the window's edge, under the spinner).
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
    // Wider by the text's indent, so the explanation keeps its lines, at
    // the measure of a compact dialog with a larger font.
    readonly property real textIndent: busyIndicator.implicitWidth + statusRow.spacing
    defaultWidth: Math.max(Kirigami.Units.gridUnit * 26 + root.textIndent, root.measureWidth + root.textIndent,
                           root.minimumWidth)

    onRejected: root.stopQuittingRequested()

    footer: Components.DialogFooter {

        id: dialogButtons
        primary: tagEditorQuitWaitKeepWaitingButton
        visible: root.offerChoice

        Button {
            id: tagEditorQuitWaitKeepWaitingButton
            objectName: "tagEditorQuitWaitKeepWaitingButton"
            text: I18n.i18nc("@action:button", "Keep Waiting")
            icon.name: "chronometer"
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
        id: statusRow
        Layout.fillWidth: true
        spacing: 8

        // 22 px beside the line with the default font, and larger with a
        // larger one.
        BusyIndicator {
            id: busyIndicator
            running: root.visible
            implicitWidth: root.uiPalette.scaledSize(Kirigami.Units.iconSizes.smallMedium)
            implicitHeight: root.uiPalette.scaledSize(Kirigami.Units.iconSizes.smallMedium)
        }

        Label {
            objectName: "tagEditorQuitWaitStatus"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.weight: Font.DemiBold
            textFormat: Text.PlainText
            text: I18n.i18ncp("@info:status", "Ferrous quits once the tag editor has written %1 file.",
                "Ferrous quits once the tag editor has written %1 files.", root.files.length)
        }
    }

    // The consequence, beside the decision.
    Label {
        objectName: "tagEditorQuitWaitMessage"
        Layout.fillWidth: true
        Layout.leftMargin: root.textIndent
        visible: root.offerChoice
        wrapMode: Text.WordWrap
        color: root.uiPalette.textSecondary
        textFormat: Text.PlainText
        text: I18n.i18nc("@info", "Quit Anyway stops waiting and may lose changes not yet written.")
    }

    Label {
        objectName: "tagEditorQuitWaitFiles"
        Layout.fillWidth: true
        // Under the status's text, not the spinner.
        Layout.leftMargin: root.textIndent
        visible: root.files.length > 0
        wrapMode: Text.WordWrap
        color: root.uiPalette.textSecondary
        textFormat: Text.PlainText
        text: root.files.slice(0, root.shownFileCount).join("\n")
            + (root.files.length > root.shownFileCount
                ? "\n" + I18n.i18ncp("@info more files than the list shows", "and %1 more", "and %1 more",
                    root.files.length - root.shownFileCount)
                : "")
    }

    Label {
        objectName: "tagEditorQuitWaitExplanation"
        Layout.fillWidth: true
        Layout.leftMargin: root.textIndent
        visible: root.offerChoice
        wrapMode: Text.WordWrap
        color: root.uiPalette.textSecondary
        textFormat: Text.PlainText
        text: I18n.i18nc("@info", "This is taking longer than it should, perhaps on an unresponsive network "
            + "share. No file is left half-written.")
    }
}
