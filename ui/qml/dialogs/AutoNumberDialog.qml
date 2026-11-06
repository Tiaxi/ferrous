// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/KeyUtils.js" as KeyUtils

Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot

    readonly property int maximumNumber: 9999
    readonly property bool startingTrackValid: startingTrackField.acceptableInput
    readonly property bool startingDiscValid: !writeDiscCheck.checked || startingDiscField.acceptableInput
    readonly property bool inputValid: root.startingTrackValid && root.startingDiscValid

    function startingNumber(field) {
        return Number.parseInt(field.text, 10)
    }

    // Applies only valid start numbers, so mistyped text never numbers from 0.
    function apply() {
        if (!root.inputValid) {
            return false
        }
        root.tagEditorApi.autoNumber(
            root.startingNumber(startingTrackField),
            writeDiscCheck.checked ? root.startingNumber(startingDiscField) : 1,
            writeDiscCheck.checked,
            writeTotalsCheck.checked,
            resetOnFolderCheck.checked,
            resetOnDiscCheck.checked)
        root.close()
        return true
    }

    objectName: "autoNumberDialog"
    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: Math.min(Kirigami.Units.gridUnit * 26, root.windowRoot.width - 2 * Kirigami.Units.gridUnit)
    title: I18n.i18nc("@title:window", "Auto Number")

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

    footer: Components.DialogFooter {

        id: dialogButtons
        primary: autoNumberApplyButton
        Button {
            id: autoNumberApplyButton
            objectName: "autoNumberApplyButton"
            text: I18n.i18nc("@action:button", "Number Tracks")
            icon.name: "format-list-ordered"
            enabled: root.inputValid
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: root.apply()
        }
        Button {
            objectName: "autoNumberCancelButton"
            text: I18n.i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.close()
        }
    }

    contentItem: ColumnLayout {
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, KeyUtils.popupTakesKeys(root)) }

        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.uiPalette.textSecondary
            text: I18n.i18nc("@label", "Number the selected rows, or all rows if none are selected, using the current table order.")
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true

            TextField {
                id: startingTrackField
                Kirigami.FormData.label: I18n.i18nc("@label:textbox", "First track:")
                objectName: "autoNumberStartingTrackField"
                Layout.fillWidth: true
                text: "1"
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: root.maximumNumber }
            }
            Label {
                objectName: "autoNumberStartingTrackError"
                Layout.fillWidth: true
                visible: !root.startingTrackValid
                wrapMode: Text.WordWrap
                color: root.uiPalette.negative
                text: I18n.i18nc("@info", "Enter a starting track from 1 to %1.", root.maximumNumber)
            }

            TextField {
                id: startingDiscField
                Kirigami.FormData.label: I18n.i18nc("@label:textbox", "First disc:")
                objectName: "autoNumberStartingDiscField"
                Layout.fillWidth: true
                text: "1"
                enabled: writeDiscCheck.checked
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: root.maximumNumber }
            }
            Label {
                objectName: "autoNumberStartingDiscError"
                Layout.fillWidth: true
                visible: !root.startingDiscValid
                wrapMode: Text.WordWrap
                color: root.uiPalette.negative
                text: I18n.i18nc("@info", "Enter a starting disc from 1 to %1.", root.maximumNumber)
            }
        }

        CheckBox {
            id: writeDiscCheck
            objectName: "autoNumberWriteDiscCheck"
            checked: false
            text: I18n.i18nc("@option:check", "Write disc numbers")
        }
        CheckBox {
            id: writeTotalsCheck
            checked: false
            text: I18n.i18nc("@option:check", "Write totals")
        }
        CheckBox {
            id: resetOnFolderCheck
            checked: false
            text: I18n.i18nc("@option:check", "Reset track numbers on folder or section change")
        }
        CheckBox {
            id: resetOnDiscCheck
            checked: false
            enabled: writeDiscCheck.checked
            text: I18n.i18nc("@option:check", "Reset track numbers when disc changes in current values")
        }
    }
}
