// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

// The new name of each selected file, and why any cannot be renamed, before
// anything is renamed. Cancel and Escape rename nothing.
Dialog {
    id: root

    required property var uiPalette
    required property var windowRoot
    // TagEditorController::renamePreviewReady's rows and error.
    property var rows: []
    property string errorText: ""
    readonly property int renameCount: root.countRows(function(row) {
        return row.ok && !row.unchanged
    })
    readonly property int refusedCount: root.countRows(function(row) { return !row.ok })

    signal renameRequested()

    function countRows(predicate) {
        let count = 0
        for (let i = 0; i < root.rows.length; ++i) {
            if (predicate(root.rows[i])) {
                ++count
            }
        }
        return count
    }

    function summaryText() {
        if (root.errorText.length > 0) {
            return I18n.i18nc("@info", "The new names could not be worked out: %1", root.errorText)
        }
        let text = I18n.i18ncp("@info", "%1 file will be renamed.", "%1 files will be renamed.", root.renameCount)
        if (root.refusedCount > 0) {
            text += " " + I18n.i18ncp("@info", "%1 file can't be renamed and keeps its name.",
                "%1 files can't be renamed and keep their names.", root.refusedCount)
        }
        return text
    }

    objectName: "renamePreviewDialog"
    parent: Overlay.overlay
    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: Math.min(root.windowRoot.width - 80, 760)
    height: Math.min(root.windowRoot.height - 80, 520)
    title: I18n.i18nc("@title:window", "Rename Files")
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        radius: 6
        color: root.uiPalette.uiPaneColor
        border.color: root.uiPalette.uiBorderColor
    }

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            objectName: "renamePreviewSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.summaryText()
            color: root.errorText.length > 0 ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
        }

        ListView {
            id: previewList
            objectName: "renamePreviewList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.rows
            spacing: 4
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: ColumnLayout {
                required property var modelData
                width: ListView.view.width - 12
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: modelData.unchanged
                        ? I18n.i18nc("@item file name", "%1 (keeps its name)", modelData.oldName)
                        : I18n.i18nc("@item old file name, new file name", "%1 → %2",
                            modelData.oldName, modelData.newName || "?")
                    color: modelData.ok
                        ? (modelData.unchanged ? root.uiPalette.uiMutedTextColor : Kirigami.Theme.textColor)
                        : Kirigami.Theme.negativeTextColor
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    visible: !modelData.ok
                    wrapMode: Text.Wrap
                    text: modelData.error || ""
                    color: Kirigami.Theme.negativeTextColor
                    font: Kirigami.Theme.smallFont
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                objectName: "renamePreviewCancelButton"
                text: I18n.i18nc("@action:button", "Cancel")
                onClicked: root.reject()
            }
            Button {
                objectName: "renamePreviewRenameButton"
                text: I18n.i18ncp("@action:button", "Rename %1 File", "Rename %1 Files", root.renameCount)
                enabled: root.renameCount > 0 && root.errorText.length === 0
                highlighted: true
                onClicked: {
                    root.close()
                    root.renameRequested()
                }
            }
        }
    }
}
