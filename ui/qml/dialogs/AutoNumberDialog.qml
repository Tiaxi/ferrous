// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

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
    width: 420
    title: I18n.i18nc("@title:window", "Auto Number")
    standardButtons: Dialog.NoButton

    background: Rectangle {
        radius: 16
        color: root.uiPalette.uiPaneColor
        border.color: root.uiPalette.uiBorderColor
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.uiPalette.uiMutedTextColor
            text: I18n.i18nc("@label", "Number the selected rows, or all rows if none are selected, using the current table order.")
        }

        GridLayout {
            columns: 2
            columnSpacing: 10
            rowSpacing: 8
            Layout.fillWidth: true

            Label { text: I18n.i18nc("@label", "Starting Track") }
            TextField {
                id: startingTrackField
                objectName: "autoNumberStartingTrackField"
                Layout.fillWidth: true
                text: "1"
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: root.maximumNumber }
            }
            Label {
                objectName: "autoNumberStartingTrackError"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                visible: !root.startingTrackValid
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.negativeTextColor
                text: I18n.i18nc("@info", "Enter a starting track from 1 to %1.", root.maximumNumber)
            }

            Label { text: I18n.i18nc("@label", "Starting Disc") }
            TextField {
                id: startingDiscField
                objectName: "autoNumberStartingDiscField"
                Layout.fillWidth: true
                text: "1"
                enabled: writeDiscCheck.checked
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: root.maximumNumber }
            }
            Label {
                objectName: "autoNumberStartingDiscError"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                visible: !root.startingDiscValid
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.negativeTextColor
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

        RowLayout {
            Layout.fillWidth: true

            Item { Layout.fillWidth: true }

            Button {
                text: I18n.i18nc("@action:button", "Cancel")
                onClicked: root.close()
            }
            Button {
                objectName: "autoNumberApplyButton"
                text: I18n.i18nc("@action:button", "Apply")
                enabled: root.inputValid
                onClicked: root.apply()
            }
        }
    }
}
