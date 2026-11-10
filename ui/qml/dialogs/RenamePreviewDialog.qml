// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/KeyUtils.js" as KeyUtils

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
    width: Math.min(root.windowRoot.width - 2 * Kirigami.Units.gridUnit, Kirigami.Units.gridUnit * 42)
    // As tall as the list, up to a limit.
    height: Math.min(root.windowRoot.height - 2 * Kirigami.Units.gridUnit, Kirigami.Units.gridUnit * 29,
        root.implicitHeight)
    title: I18n.i18nc("@title:window", "Rename Files")
    closePolicy: Popup.CloseOnEscape

    // The keyboard in the content, whose Enter presses the default button,
    // from the moment the dialog shows.
    Components.PopupInitialFocus {
        popup: root
        item: root.contentItem
    }

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

    footer: Components.DialogFooter {

        id: dialogButtons
        primary: renamePreviewRenameButton
        Button {
            id: renamePreviewRenameButton
            objectName: "renamePreviewRenameButton"
            text: I18n.i18ncp("@action:button", "Rename %1 File", "Rename %1 Files", root.renameCount)
            icon.name: "edit-rename"
            enabled: root.renameCount > 0 && root.errorText.length === 0
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: {
                root.close()
                root.renameRequested()
            }
        }
        Button {
            objectName: "renamePreviewCancelButton"
            text: I18n.i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    contentItem: ColumnLayout {
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, KeyUtils.popupTakesKeys(root)) }

        spacing: 10

        Label {
            objectName: "renamePreviewSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.summaryText()
            visible: root.rows.length > 0 || root.errorText.length > 0
            color: root.errorText.length > 0 ? root.uiPalette.negative : root.uiPalette.text
        }

        // Nothing to list: a message alone, in the middle. Plain labels: a
        // placeholder message's selectable text brings a Copy action whose
        // Ctrl+C the window's own Copy has.
        ColumnLayout {
            objectName: "renamePreviewEmpty"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Kirigami.Units.gridUnit
            Layout.bottomMargin: Kirigami.Units.gridUnit
            visible: root.rows.length === 0 && root.errorText.length === 0
            spacing: Kirigami.Units.largeSpacing

            Item { Layout.fillHeight: true }

            Kirigami.Icon {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: Kirigami.Units.iconSizes.huge
                implicitHeight: Kirigami.Units.iconSizes.huge
                source: "edit-rename"
                opacity: 0.5
                Accessible.ignored: true
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                // A heading for screen readers, as Kirigami's placeholder
                // message gives its text.
                Accessible.role: Accessible.Heading
                text: I18n.i18nc("@info", "No file is renamed")
                font.pointSize: root.uiPalette.basePointSize * 1.125
                font.weight: Font.DemiBold
                color: root.uiPalette.textSecondary
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: I18n.i18nc("@info", "The selected files already have the names their tags give.")
                color: root.uiPalette.textSecondary
            }

            Item { Layout.fillHeight: true }
        }

        ListView {
            id: previewList
            objectName: "renamePreviewList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: contentHeight
            visible: root.rows.length > 0
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
                            modelData.oldName, modelData.newName || "")
                    color: modelData.ok
                        ? (modelData.unchanged ? root.uiPalette.textSecondary : root.uiPalette.text)
                        : root.uiPalette.negative
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    visible: !modelData.ok
                    wrapMode: Text.Wrap
                    text: modelData.error || ""
                    color: root.uiPalette.negative
                    font.pointSize: root.uiPalette.smallPointSize
                }
            }
        }
    }
}
